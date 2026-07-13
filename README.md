# ESP32-S3 On-Device AG-UI Voice Client

Custom **ESP-IDF 5.5.x** firmware that turns a **Waveshare ESP32-S3-Touch-AMOLED-1.8** into a
*first-class [AG-UI](https://docs.ag-ui.com) client* — not a thin voice satellite.

The device captures mic audio and streams it to **[Soniox](https://soniox.com)** for real-time
speech-to-text, is itself the **AG-UI client** (it POSTs `RunAgentInput` and consumes the SSE
event stream directly on-device), renders the conversation plus live agent activity on the
1.8″ AMOLED via **LVGL 8.4**, and **speaks the reply back** with streaming Soniox text-to-speech.
STT/TTS are also pluggable: **[Deepgram](https://deepgram.com)** (Listen / Speak v1) is available
as an alternate provider from the captive portal (see [docs/speech-providers.md](docs/speech-providers.md)).
Because the AG-UI client lives on the device, agent events (`TOOL_CALL_*`, reasoning, run
lifecycle) drive the screen, and the board exposes its own sensors / screen / clock back to the
agent as tools and ambient context.

> **Status:** v1 functional and hardware-verified. Streaming STT, the AG-UI text client, the LVGL
> chat + live-status UI, **spoken replies (streaming TTS with push-to-talk barge-in)**, **ambient
> context** (time + battery + selected voice), the **`set_timer` client tool** (with an on-device
> ringing alarm), a **captive-portal voice picker**, **volume buttons**, **eyes-free touch-to-talk**,
> a **user-uploadable alarm graphic**, a **configurable screen-blank timeout** (or always-on), an
> **idle screensaver** that gently pulses the uploaded image, and **idle power saving** are all
> implemented and working on hardware. HITL interrupts and Soniox ephemeral keys remain (see
> [Roadmap](#roadmap)).

```
mic ─ES8311/I²S(16k s16le)─▶ Soniox STT (streaming WSS) ─▶ live transcript
   ─▶ AG-UI: POST RunAgentInput{messages, context} ─▶ SSE event stream
   ─▶ LVGL: chat bubbles (TEXT_*) · ephemeral status (REASONING_*/TOOL_CALL_*/RUN_*)
   ─▶ reply text ─▶ Soniox TTS (streaming WSS) ─▶ ES8311 speaker  (barge-in to interrupt)
   ─▶ client tools run on-device (set_timer → ringing alarm)
```
(Same pipeline with Deepgram when that provider is selected.)

---

## Hardware

**Board:** [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8)

| | |
|---|---|
| **MCU** | ESP32-S3 (Xtensa LX7 dual-core, 240 MHz), 16 MB flash, 8 MB octal PSRAM @ 80 MHz |
| **Display** | 1.8″ AMOLED, **368 × 448 px**, QSPI |
| **Audio** | ES8311 codec + dual digital mics (16 kHz mono capture) + speaker out |
| **Sensors** | QMI8658 6-axis IMU · AXP2101 PMIC · PCF85063 RTC |
| **Input** | Capacitive touch (long-press anywhere = talk) · top **BOOT** button · **PWR** key |
| **Other** | SD/MMC slot · 3.7 V Li battery (MX1.25) · USB-C (native USB Serial/JTAG) |

Two hardware revisions are auto-detected at runtime by probing the touch controller over I²C —
**V1** (SH8601 display + FT3168 touch @ 0x38) and **V2** (CO5300 + CST816 @ 0x15). One binary
serves both. Full pinout is in [CLAUDE.md](CLAUDE.md).

---

## Architecture

```
        ┌──────────────────── ESP32-S3 (ESP-IDF, LVGL) ─────────────────────┐
 mic ─ES8311─I²S─▶ audio task ─ring─▶ soniox_client ════ wss ════▶ Soniox STT
                                            │ transcript (partial + is_final)
 QMI8658/AXP2101/PCF85063 ─▶ device_tools.context ─┐
                                                   ▼
                          agui_client ═══ https POST RunAgentInput ═══▶ AG-UI agent
                                      ◀═════════════ SSE events ════════
                                            │  (event router)
                  ┌─────────────────────────┼───────────────────────────┐
                  ▼            ▼             ▼              ▼             ▼
              chat bubbles  ephemeral    client tools   reply text    idle
              (TEXT_*)      status       (set_timer)    ─▶ TTS ─▶ spk  timers
                                                        soniox_tts_client ══ wss ══▶ Soniox TTS
        └────────────────────────────── AMOLED + speaker ───────────────────┘
```

Secrets live in NVS. The mic→STT leg runs first (Soniox closes on end-of-speech), then the AG-UI
run opens; the **TTS** stream overlaps the agent SSE so the reply is spoken as it arrives. mbedTLS
buffers are kept in **PSRAM**, which is what lets the TTS WSS session coexist with the agent stream
on a RAM-tight build. There is no acoustic barge-in (single ES8311, no echo-reference channel);
instead, **starting a new turn cancels any in-progress speech** — push-to-talk turn-taking.

The on-device AG-UI client **vendors and extends the community C++ AG-UI SDK**
(`ag-ui-protocol/ag-ui` → `sdks/community/c++`) under the `agui_sdk` component: libcurl →
`esp_http_client` (streaming SSE), nlohmann/json → cJSON, plus device extensions (per-run ambient
`context`, `REASONING_*` events, interrupt → resume, client-tool dispatch). `agui_client` is a thin
`extern "C"` shim over the SDK's `HttpAgent` + an `IAgentSubscriber`. Local deltas to the vendored
SDK are tracked in [agui_sdk/PATCHES.md](esp32-agui/components/agui_sdk/PATCHES.md).

---

## Repository layout

```
esp32-agui/                      ← repo root
├── esp32-agui/                  ← the ESP-IDF project (self-contained; build this)
│   ├── main/                    ← app entry + push-to-talk turn state machine
│   ├── components/              ← first-party components (below) + vendored board BSP/drivers
│   ├── sdkconfig.defaults       ← target esp32s3, TLS 1.3, PSRAM, power mgmt, LVGL config
│   └── partitions.csv           ← nvs · 3 MB app · 256 KB "alarmimg" (uploaded alarm graphic)
├── docs/
│   ├── esp32-agui-plan.md       ← full design: components, APIs, state machine, build phases
│   ├── flashing.md              ← flash workflow + NVS-wipe recovery
│   └── soniox-rt-protocol.md    ← verified Soniox real-time WSS protocol notes (STT + TTS)
├── .claude/commands/            ← /idf-build /idf-flash /idf-monitor /idf-qemu /idf-size /idf-docs
├── CLAUDE.md                    ← project guide (hardware, toolchain, layout, conventions)
└── README.md                    ← you are here
```

### First-party firmware components ([esp32-agui/components/](esp32-agui/components/))

| Component | Responsibility |
|---|---|
| [`agui_client`](esp32-agui/components/agui_client/) | Thin `extern "C"` shim over the vendored AG-UI SDK: POST `RunAgentInput` → handler callbacks |
| [`agui_sdk`](esp32-agui/components/agui_sdk/) | Vendored + ESP-ported community C++ AG-UI SDK (streaming SSE parser, event router) |
| [`speech_stt`](esp32-agui/components/speech_stt/) / [`speech_tts`](esp32-agui/components/speech_tts/) | Facades over Deepgram or Soniox (see [docs/speech-providers.md](docs/speech-providers.md)) |
| [`deepgram_stt`](esp32-agui/components/deepgram_stt/) / [`deepgram_tts`](esp32-agui/components/deepgram_tts/) | Deepgram Listen / Speak v1 WSS backends |
| [`soniox_client`](esp32-agui/components/soniox_client/) | ES8311 mic capture → Soniox WSS streaming STT → partial/final transcript callbacks |
| [`soniox_tts_client`](esp32-agui/components/soniox_tts_client/) | Reply text → Soniox WSS streaming TTS → PCM → ES8311 speaker (cancelable for barge-in) |
| [`chat_ui`](esp32-agui/components/chat_ui/) | LVGL chat bubbles, status line, touch-to-talk, configurable screen power saver, ringing-alarm overlay (uploaded graphic), idle screensaver |
| [`net_prov`](esp32-agui/components/net_prov/) | Multi-SSID WiFi connect + auto-reconnect + SoftAP captive-portal provisioning (keys/TZ/voice/timeout + alarm-image upload) |
| [`alarm_img`](esp32-agui/components/alarm_img/) | Store/load the user-uploaded alarm graphic (240×240 RGB565) in a dedicated flash partition, staged via PSRAM |
| [`app_cfg`](esp32-agui/components/app_cfg/) | Tiny NVS-backed config/secret store (Soniox key, AG-UI URL + bearer, TZ, voice, volume, screen timeout, idle-anim flag) |
| [`device_tools`](esp32-agui/components/device_tools/) | Ambient context (battery/time/voice) + client-tool registry (`set_timer`) |
| [`esp32_s3_touch_amoled_1_8`](esp32-agui/components/esp32_s3_touch_amoled_1_8/) | Board BSP (display/touch/audio bring-up); vendored from the Waveshare LVGL example |
| `board_variant`, `esp_lcd_*`, `espressif__*` | Vendored display/touch drivers + ESP component-registry deps |

---

## Toolchain — ESP-IDF 5.5.x **only**

The managed components cap at `idf <6.0` (e.g. `espressif/es8311` requires `>=4.4,<6.0`), so
**ESP-IDF 6.x breaks dependency solving**. If you see
`ERROR: Version solving failed: no versions of idf match >=5.5.0,<6.0.0`, you're on the wrong IDF.

Install **ESP-IDF 5.5.x** with the [official getting-started flow](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/),
or directly:

```bash
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
```

Then source the environment in every shell you build from:

```bash
. ~/esp/esp-idf/export.sh
```

Target chip is always **esp32s3** — it comes from `sdkconfig.defaults`, so **don't run
`idf.py set-target`** (it wipes `sdkconfig`).

---

## Build, flash, run

All commands run from inside the project dir.

```bash
cd esp32-agui
idf.py build
```

### Flashing

Connect the board with a USB-C **data** cable and flash over the chip's native USB Serial/JTAG:

```bash
idf.py -p <PORT> flash monitor
```

`<PORT>` is e.g. `/dev/ttyACM0` (Linux), `/dev/cu.usbmodem*` (macOS), or `COMx` (Windows). If the
port doesn't enumerate, hold **BOOT**, tap **RESET**, then release **BOOT** to force ROM
download mode.

Working **inside the devcontainer** (no USB there)? Flash and monitor over a serial-over-TCP
bridge instead — `esp_rfc2217_server` on the host + `tools/monitor-bridge.sh`; procedure and
gotchas in [docs/flashing.md](docs/flashing.md#flashing-from-inside-the-devcontainer-rfc2217-bridge).

> ⚠️ **Routine reflash = app alone at `0x10000`** (`build/esp32_agui.bin`) — it leaves NVS intact.
> Flashing a *merged* image at `0x0` pads the NVS region (`0x9000`) with `0xFF` and **wipes your
> saved WiFi/keys**. When the **partition table changes** (e.g. the `alarmimg` partition was added),
> flash it once too: a normal `idf.py flash` writes bootloader + partition-table (`0x8000`) + app and
> still leaves `nvs` untouched. See [docs/flashing.md](docs/flashing.md) for the recovery procedure
> and download-mode entry.

**Verify the running build** from the boot log's `ELF file SHA256:` line — the compile-time
timestamp is stale on incremental builds, so the ELF hash is the reliable identity check.

### Monitor

Console is routed to the chip's native **USB Serial/JTAG** (the USB-C port, VID `0x303a`/PID
`0x1001`), so `idf.py monitor` shows `ESP_LOG` output over USB-C.

### QEMU

`idf.py qemu` emulates **CPU / RAM / UART only** — the AMOLED, touch, I²C sensors, SD, and codec
are not emulated. Useful for boot / app-logic / networking, not the UI.

---

## First-run setup & usage

1. **Provision.** With no saved credentials, the device starts a SoftAP captive portal named
   **`AMOLED-setup`**. Join it from a phone, and the form lets you set:
   - WiFi SSID + password
   - **Soniox API key** (or switch **Speech provider** to Deepgram and enter a Deepgram key)
   - **AG-UI endpoint URL** (+ optional bearer token)
   - **Time zone** (POSIX `TZ` string, for the agent's ambient time context)
   - **TTS voice** (Soniox `tts-rt-v1` voices, default **Adrian**; Deepgram Aura-2 voices when that provider is selected)
   - **Screen blank timeout** (seconds; default 60, `0` = always on)
   - **Idle animation** (checkbox; gently pulse the uploaded alarm image when idle)
   - **Alarm image** (file picker; any image is cropped in-browser to 240×240, converted to
     RGB565, and uploaded — no on-device decode)

   Only the fields you fill in are overwritten; the confirmation page echoes the saved values.
   Everything is stored in NVS, and the device connects.

2. **Talk.** **Long-press anywhere on the screen** (eyes-free), or **hold the top BOOT button**, to
   talk — the Soniox session opens only while held, with a live transcript scrolling across the top
   status line. **Release** to send the utterance to your AG-UI agent. The reply streams into a chat
   bubble *and is spoken aloud* while the status line shows what the agent is doing
   (`Thinking… → Reasoning… → Using web_search… →` reply). **Start a new turn to barge in** and cut
   off the current spoken reply.

3. **Volume.** **Tap** the BOOT button to turn the volume **up**; **short-press** the PWR key to
   turn it **down**. The level is persisted to NVS.

4. **Reconfigure.** **Double-tap** the BOOT button to reopen the setup portal and change the
   endpoint / keys / voice / time zone without reflashing.

5. **Tools & alarms.** When the agent calls `set_timer`, the device runs an idle countdown and then
   **rings** — an escalating beep with a full-screen alert; **tap the screen to dismiss**. The alert
   shows your **uploaded alarm graphic** (pulsing) if one is set, otherwise a flashing red ring.

The display **blanks after a configurable timeout** (default 60 s, or `0` = always on) to save power
and **wakes on any touch or button press**. With **idle animation** enabled (and an alarm image
uploaded), it instead shows a calm screensaver that pulses the image — 5 s blank → 5 s fade-in →
5 s full → 5 s fade-out, looping. On battery, plain blanking also sheds WiFi + codec and drops the
CPU into light sleep (the next turn reconnects in ~1–3 s); the screensaver keeps the device awake, so
it's best on USB power. A long PWR-key hold powers the device off (AXP2101).

Configuration and secrets are stored in NVS (`appcfg` namespace for the Soniox key / AG-UI URL +
token / TZ / voice / volume; `netprov` for WiFi). Soniox **ephemeral keys** (mint-on-demand) are
planned (see Roadmap).

---

## Roadmap

| Phase | Goal | Status |
|---|---|---|
| **P0** Environment + WiFi | IDF 5.5 pinned; `net_prov` multi-SSID connect + auto-reconnect | ✅ |
| **P0.5** Provisioning | SoftAP captive portal (`AMOLED-setup`) + web form (WiFi/keys/TZ/voice) | ✅ |
| **P1** Soniox STT | `soniox_client`: ES8311 capture → WSS → transcript | ✅ |
| **P2** AG-UI text | `agui_client`/`agui_sdk` (RunAgentInput + SSE + `TEXT_*`) | ✅ |
| **P3** Chat UI | `chat_ui` chat list + streaming + BOOT/touch push-to-talk | ✅ |
| **P4** Status | `REASONING_*`/`TOOL_CALL_*`/`RUN_*` → live status line | ✅ |
| **P5** Context | ambient battery + local time + selected voice → per-run `context` | ✅ |
| **P7** Tools | `set_timer` (idle countdown → ringing alarm) round-trip | ✅ |
| **TTS** Spoken replies | `soniox_tts_client`: streaming reply audio + barge-in | ✅ |
| — UX | voice picker, volume buttons, eyes-free touch-to-talk, uploadable alarm graphic, configurable screen timeout, idle screensaver | ✅ |
| — Latency | CPU pinned to 240 MHz per turn (faster TLS handshakes) | ✅ |
| **P6** Interrupt | detect HITL → render-by-`response_schema` → resume; `show_qr` | ◻ planned |
| **P8** Hardening | Soniox ephemeral keys, reconnect/backoff, PSRAM tuning, error states | ◻ planned |

Backlog (post-P8): A2UI generative-UI rendering, wake-word turn control, `device_motion` context,
`set_alarm` (RTC), v2 tools (`display_card`/`render_chart`/`ble_*`/`sd_*`), `STATE_*` + JSON-Patch
shared state.

The full design — component APIs, runtime state machine, AG-UI event→UI mapping, and per-phase
acceptance criteria — is in [docs/esp32-agui-plan.md](docs/esp32-agui-plan.md).

---

## Credits & licenses

This project is licensed under the [MIT License](LICENSE).

It builds on, ports, or interoperates with:

- **AG-UI protocol** ([ag-ui-protocol/ag-ui](https://github.com/ag-ui-protocol/ag-ui)) — the
  on-device client vendors and ESP-ports the community **C++ SDK** (`sdks/community/c++`); local
  deltas are documented in [agui_sdk/PATCHES.md](esp32-agui/components/agui_sdk/PATCHES.md).
- **Soniox** real-time **STT** (model `stt-rt-v5`) and **TTS** (model `tts-rt-v1`) over WSS —
  protocol notes in [docs/soniox-rt-protocol.md](docs/soniox-rt-protocol.md).
- **Waveshare ESP32-S3-Touch-AMOLED-1.8 BSP + drivers**
  ([waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8))
  — vendored under `esp32-agui/components/`.
- **Espressif ESP-IDF**, **LVGL 8.4**, and ESP component-registry packages
  (`esp_lcd_sh8601`/`esp_lcd_co5300`, `esp_lcd_touch_*`, `button`, `esp_codec_dev`,
  `esp_websocket_client`, …).

Driver/API questions can be researched against the upstream repos via the DeepWiki MCP
(`/idf-docs <question>`) — see [CLAUDE.md](CLAUDE.md) for the configured sources.
