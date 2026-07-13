# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is the **ESP-IDF firmware project** for the on-device AG-UI voice client. It is the buildable
root (`idf.py` runs here). The **parent guide** [`../CLAUDE.md`](../CLAUDE.md) covers the hardware,
the **ESP-IDF 5.5.x-only** toolchain constraint, flashing (incl. the devcontainer RFC2217 bridge),
and DeepWiki driver lookup — read it for those; it is not repeated here. The parent doc frames the
client as a "roadmap"; it is now implemented in this directory. Design docs live in
[`../docs/`](../docs/) (`esp32-agui-plan.md`, `speech-providers.md`, `flashing.md`).

## Build / flash / test

Source the IDF env first (`. ~/esp/esp-idf/export.sh`), then from **this directory**:

- **Build:** `idf.py build` — target `esp32s3` comes from `sdkconfig.defaults`. Do **not** run
  `idf.py set-target` (it wipes `sdkconfig`). Slash-commands `/idf-build` `/idf-flash` `/idf-monitor`
  `/idf-qemu` `/idf-size` wrap the common invocations.
- **Flash + monitor:** `idf.py -p <PORT> flash monitor` (`/dev/cu.usbmodem*` on macOS).
- **QEMU:** boots app logic + networking only — no AMOLED/touch/codec/I²C. Not for UI work.
- **AG-UI SDK host tests:** `components/agui_sdk/test/run_host_tests.sh` — plain `g++`, no ESP-IDF or
  hardware. Round-trips the device SDK extensions (REASONING_*/interrupt/resume). **Run this after
  re-syncing the vendored SDK** (see below); it fails loudly if a delta was dropped.

Partitions (`partitions.csv`): `factory` app = 3 MB; `alarmimg` (custom type `0x40`, 0x40000) holds
the portal-uploaded alarm graphic. A normal `idf.py flash` preserves `nvs` (saved WiFi/keys).

## Component architecture

First-party components under `components/` (managed deps live in `managed_components/`, vendored BSP
+ drivers are also under `components/`). The design is layered — `main` never talks to a cloud
provider or the raw SDK directly, only to facades:

```
main/esp32_agui_main.c   app entry, PTT state machine, audio/beep/alarm, low-power (see below)
  ├─ net_prov           WiFi (multi-SSID) + SoftAP captive portal ("AMOLED-setup") + SNTP/HTTPS clock
  ├─ app_cfg            NVS string store (namespace "appcfg"): keys, secrets, prefs (APP_CFG_* macros)
  ├─ speech_cfg         resolves active provider + API key from NVS; caches, invalidate on portal save
  ├─ speech_stt ──────► soniox_client | deepgram_stt   streaming STT facade (session start/stop/finalize)
  ├─ speech_tts ──────► soniox_tts_client | deepgram_tts  streaming TTS facade (open/feed/finish + speak)
  ├─ agui_client ─────► agui_sdk                        AG-UI client (see "AG-UI SDK" below)
  ├─ device_tools       tool registry + impls (set_timer/set_alarm/show_qr/show_image) + ambient context
  ├─ chat_ui            LVGL UI: chat bubbles, status pill, face overlay, screensaver, alarm overlay
  └─ alarm_img          read/write the alarmimg flash partition
```

**Speech facade (`speech_*`).** The rest of the app is provider-agnostic. `speech_stt`/`speech_tts`
dispatch to Soniox (default; real-time WSS) or Deepgram (opt-in; Listen/Speak v1 WSS) based on
`speech_provider_get()`. **Never call a provider backend (`soniox_*`, `deepgram_*`) from `main` —
always go through the facade.** Both providers use **16 kHz / s16le / mono** end-to-end (see the
ES8311 constraint below). Adding a provider: implement `*_stt`/`*_tts` with the same
session/open-feed-finish API, extend `speech_provider_t` + the portal dropdown, dispatch in
`speech_stt.c`/`speech_tts.c`. Details: [`../docs/speech-providers.md`](../docs/speech-providers.md).

**AG-UI SDK (`agui_sdk` + `agui_client`).** `agui_sdk` **vendors the upstream AG-UI community C++
SDK** (`ag-ui-protocol/ag-ui` `sdks/community/c++`, MIT) with device patches; `agui_client` is a thin
`extern "C"` shim over it (`main` is C). Ports: libcurl→`esp_http_client` (`EspHttpService`
implements the SDK's `IHttpService`), and the SDK keeps `nlohmann/json` (managed dep
`johboh__nlohmann-json`) while the shim exposes cJSON. Device extensions: per-run ambient `context`,
first-class `REASONING_*` events, Interrupt→resume, client-tool dispatch. **Re-syncing the vendor
snapshot:** all deltas are marked `// [device]` in-tree (`grep -rn "\[device\]" components/agui_sdk/src`);
follow `components/agui_sdk/PATCHES.md` and re-run the host tests. C++ components pin `-std=gnu++17`.

## Runtime model — the part you must understand before editing `main`

**One task drives everything: `ptt_task`.** There is no continuous STT session and no barge-in
audio mixer. A FreeRTOS queue (`s_ptt_q`) serializes all input events (button/touch/PWR callbacks
only flip flags + enqueue — they never block or dispatch). Event codes: `1`=press, `0`=release,
`2`=setup-portal, `3`/`4`=volume up/down.

**Turn model (push-to-talk).** Hold BOOT **or** long-press the screen → open the STT session, stream
mic, show the live transcript in the status line. Release → stop the session, assemble the utterance
(accumulated finals + last interim), and run one AG-UI turn. The button *defines the turn boundary*,
so there is no idle STT timeout and no streamed-silence garbage. BOOT **tap** = volume up, BOOT
**double-tap** = reopen the setup portal, PWR short-press = volume down.

**Agent turn + client-tool loop (`run_agent_turn`).** `agui_run()` **blocks** on `ptt_task` and
fires handlers inline. Critical rule: the `on_tool_call` handler runs *inside* `agui_run` under the
SDK lock, so it may only **record** client-tool calls into `s_pending[]` (copy strings) — never
dispatch or re-run (deadlock). After `agui_run` returns, `ptt_task` drains `s_pending[]`, executes
each tool via `device_tools_dispatch`, appends a result with `agui_tool_result`, and re-runs with
`user_text=NULL`. Bounded by `AGUI_TOOL_MAX_ITERS`. Only tools where `device_tools_is_client(name)`
is true are captured — server/agent tools are the agent's to run.

**TTS: streaming with batch fallback.** On the first speakable text delta, `run_agent_turn` opens a
live TTS stream (`speech_tts_open`/`_feed`) so audio starts before the reply finishes; it also buffers
the whole reply. If the stream never opened (delta-less reply / OOM), it falls back to
`speech_tts_speak` on the buffered text after the run.

**Barge-in (`s_responding`/`s_aborting`).** A press while a reply is in flight aborts it:
`agui_abort()` flips the SDK's atomic cancel flag (lock-free, callable from any task) and
`speech_tts_cancel()` stops playback. `on_error` checks `s_aborting` to suppress the deliberate
cancel as a non-error; the partial assistant message is dropped (`agui_drop_partial_assistant`) so it
can't poison the next run.

**Two hard concurrency invariants:**
- **Single ES8311, full-duplex.** The mic IN handle stays open; the speaker is a *second*
  `esp_codec_dev` OUT handle on the same chip. Both **must** use the same 16 kHz/16-bit/mono format
  (the shared I2S clock's last `set_fs` wins and is not auto-enforced). Opening the speaker
  soft-resets the chip and clobbers the mic ADC gain, and beep crosstalk lands in the RX DMA ring —
  the capture task re-asserts gain and drains the ring before forwarding audio. See the long comment
  above `BEEP_SR` in `main`.
- **Sequential TLS, PSRAM buffers.** STT, AG-UI, and TTS TLS sessions run **one at a time** (listen →
  respond); the device can't afford concurrent handshakes. The heartbeat logs
  `internal_max` (largest contiguous *internal* block) because TLS/lwIP send buffers need contiguous
  internal RAM — that number, not total free heap, predicts session failures.

**Low-power idle.** PM is configured with light-sleep enabled and a `NO_LIGHT_SLEEP` lock held while
active. On **battery** idle (display blanked, via the `chat_ui` power-cb) `lp_idle` sheds WiFi + the
codec and releases the lock so the CPU light-sleeps; `lp_wake` (a PTT press) reverses it. Plugged-in
stays fully connected. A separate `CPU_FREQ_MAX` lock is held only for the span of a turn (`turn_perf`)
so the three TLS handshakes run at 240 MHz. Removing this block breaks the STT upload even on good
WiFi — do not "simplify" it away.

**UI liveness.** `chat_ui` bumps a 1 Hz counter from inside the LVGL task; the heartbeat flags a
**UI STALL** if it doesn't move (historically an AMOLED brightness `tx_param` racing an LVGL flush —
see the fix in commit `f8bb8b9`).

## Config & secrets

All runtime config/secrets live in **NVS** (`app_cfg`, namespace `appcfg`; `APP_CFG_*` keys in
`components/app_cfg/include/app_cfg.h`), written by the captive portal (`net_prov`). Includes WiFi,
speech provider + API key, AG-UI URL + bearer, TZ, TTS voice/volume, screen timeout, idle-anim flag.
`main` boots straight into provisioning (opens `AMOLED-setup` SoftAP) until WiFi + a speech key +
`agui_url` are all present. Legacy `soniox_key` is still read as a fallback for the Soniox provider.

## Conventions

- Source comments tag work by build phase (**P0**–**P7**, plus **P-a/-b/-c** for the TTS/barge-in
  sub-phases). The phase map is in [`../docs/esp32-agui-plan.md`](../docs/esp32-agui-plan.md); keep
  using the same tags when extending a feature.
- `device_tools` and `chat_ui` avoid a circular dependency by having `main` wire runtime callbacks
  after init (`device_tools_set_show_image_handler`, `chat_ui_set_*_cb`) rather than one `#include`
  the other. Preserve this when adding cross-component hooks.
