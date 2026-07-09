# On-device AG-UI Voice Client — Full Plan

Status: **committed design, v1 scope.** Summary lives in [../CLAUDE.md](../CLAUDE.md); this is the detail.

## 1. Decision

Custom **ESP-IDF 5.5.x** firmware that makes the ESP32-S3-Touch-AMOLED-1.8 a *first-class AG-UI client*. The device:

- captures mic audio and streams it to **Soniox** real-time STT (on-device WSS),
- is itself the **AG-UI client** (POST `RunAgentInput` → consume the SSE event stream),
- renders chat + live agent activity + human-in-the-loop prompts on the AMOLED via **LVGL**,
- exposes its own sensors / screen / touch as **agent-callable tools** and **ambient context**.

Deliberately the heaviest route evaluated — chosen for full control of the AMOLED UX and *native* AG-UI (rich on-device UI a thin voice satellite can't surface).

### Alternatives considered
| Route | Verdict |
|---|---|
| **Xiaozhi** (factory fw; this board officially supported) | Fastest path; its own protocol/server; AG-UI would be a server `LLMProvider`. Thin device. |
| **ESPHome + Home Assistant** (most established; user's `contextablemark/home-agui-agent` already bridges HA→AG-UI) | Strong thin-device option, reuses the user's AG-UI work; but display = SH8601 bring-up risk and AG-UI collapses to a speech string (no rich on-device UI). |
| **Pipecat / LiveKit** (WebRTC) | Real-time media + first-party Soniox plugin, but board port + Developer-Preview/stale client + AG-UI bridge. |
| **Custom on-device AG-UI client** *(this plan)* | Most work, most control; device IS the AG-UI client → tool/interrupt/state events drive the screen. |

## 2. Hardware & constraints

V1 board: **SH8601** 368×448 QSPI AMOLED, **FT3168** touch, **ES8311** audio, **QMI8658** IMU, **AXP2101** PMIC, **PCF85063** RTC, SD/MMC, 8 MB octal PSRAM. (Runtime board-variant detection also handles V2 CO5300/CST816.) Full pinout in [../CLAUDE.md](../CLAUDE.md).

- **ESP-IDF 5.5.x only** — managed components cap at `idf <6.0`.
- **No barge-in:** single ES8311, no echo-reference channel → wake/PTT turn-taking (not talk-over).
- Round panel ~50 px corner radius → LVGL safe-zone inset.

## 3. Architecture

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
              chat bubbles  ephemeral    interrupt      client tools   idle
              (TEXT_*)      status       prompt (HITL)  (timer/qr)     timer
        └────────────────────────────── AMOLED ─────────────────────────────┘
```

Secrets in NVS. TLS sessions run **sequentially** (Soniox closes on end-of-speech before the AG-UI run opens), so peak is one mbedTLS session; large buffers in PSRAM.

## 4. Runtime state machine

- **IDLE** — idle screen (clock; running `set_timer` countdown if any). Awaiting wake/PTT.
- **LISTENING** — capture + stream PCM to Soniox; show partial transcript. Ends on Soniox endpoint-detection (or PTT release) → `soniox_finish()`.
- **THINKING** — final transcript → `agui_run()`; consume SSE. Ephemeral status (reasoning, tool calls); client tools execute inline → `TOOL_CALL_RESULT`.
- **RESPONDING** — stream `TEXT_MESSAGE_CONTENT` into the assistant bubble.
- **INTERRUPT** — `RUN_FINISHED outcome=interrupt` → blocking touch prompt (by `response_schema`) → resume (new run) → back to THINKING.
- **ERROR** — surface message; return to IDLE.

Conversation continuity via `threadId`.

## 5. Components

```
main/                   app orchestration: state machine, NVS config, task wiring
components/
  net_prov/             WiFi: NVS multi-SSID connect + auto-reconnect + SoftAP captive portal
  soniox_client/        real-time STT over esp_websocket_client
  agui_client/          AG-UI client (ported from ag-ui C++ SDK) + 3 device extensions
  device_tools/         tool registry + impls + ambient-context provider
  chat_ui/              LVGL: chat list, status, interrupt prompt, idle timer
  esp32_s3_touch_amoled_1_8/   BSP (reuse from examples): display/touch/audio/sensors
```

### 5.1 `soniox_client` (API sketch)
```c
typedef struct {
  const char *endpoint;     // wss://stt-rt.soniox.com/transcribe-websocket
  const char *api_key;      // ephemeral key preferred
  const char *model;        // "stt-rt-v5"
  int  sample_rate;         // 16000
  int  channels;            // 1
  bool endpoint_detection;  // true
} soniox_cfg_t;

typedef void (*soniox_token_cb)(const char *text, bool is_final, void *ctx);
typedef void (*soniox_done_cb)(const char *utterance, void *ctx);   // endpoint/finished

esp_err_t soniox_open(const soniox_cfg_t*, soniox_token_cb, soniox_done_cb, void *ctx);
esp_err_t soniox_send_pcm(const int16_t *pcm, size_t samples);      // binary frame
esp_err_t soniox_finish(void);                                      // empty frame
void      soniox_close(void);
```

### 5.2 `agui_client` (API sketch)
```c
typedef struct { const char *id, *reason, *message, *tool_call_id;
                 cJSON *response_schema; int64_t expires_at; } agui_interrupt_t;

typedef struct {
  void (*on_run_started)(void*);
  void (*on_text_delta)(const char *delta, void*);          // TEXT_MESSAGE_CONTENT
  void (*on_text_end)(void*);
  void (*on_reasoning)(const char *delta, bool active, void*);
  void (*on_tool_call)(const char *id, const char *name, const char *args_json, void*);
  void (*on_interrupt)(const agui_interrupt_t*, void*);     // RUN_FINISHED outcome=interrupt
  void (*on_run_finished)(void*);
  void (*on_error)(const char *msg, void*);
  // v2: on_state_snapshot / on_state_delta (RFC 6902)
} agui_handlers_t;

typedef struct { const char *endpoint, *auth_bearer, *thread_id; } agui_cfg_t;

esp_err_t agui_run(const agui_cfg_t*, const char *user_text,
                   const cJSON *context,                    // [{description,value}] ambient
                   const cJSON *tools,                      // advertised client tools
                   const cJSON *resume,                     // NULL unless resuming an interrupt
                   const agui_handlers_t*, void *ctx);
esp_err_t agui_tool_result(const char *tool_call_id, const cJSON *result);
```

### 5.3 `device_tools` (API sketch)
```c
cJSON *device_context_build(void);   // motion(QMI8658)+battery(AXP2101)+time(PCF85063)
cJSON *device_tools_manifest(void);  // JSON-schema list for RunAgentInput.tools

typedef esp_err_t (*device_tool_fn)(const cJSON *args, cJSON **result);
void device_tools_register(const char *name, const cJSON *schema, device_tool_fn);
// builtins: set_timer, set_alarm, show_qr
```

### 5.4 `chat_ui` (API sketch)
```c
void  chat_ui_add_user(const char *text);
lv_obj_t *chat_ui_begin_assistant(void);
void  chat_ui_append_assistant(const char *delta);
void  chat_ui_status(const char *text);            // ephemeral
void  chat_ui_clear_status(void);
void  chat_ui_show_qr(const char *data);           // lv_qrcode
void  chat_ui_idle_timer(int seconds_left, const char *label);
// interrupt: build widgets from response_schema, return answer via callback
typedef void (*chat_ui_answer_cb)(const cJSON *answer, void*);
void  chat_ui_prompt(const char *message, const cJSON *response_schema,
                     int64_t expires_at, chat_ui_answer_cb, void *ctx);
```

### 5.5 `net_prov` (API sketch)
Pattern adapted from `app-pixels/ai-chat` (`wifi_try_connect()` multi-SSID + early-abort), ported
to `esp_wifi`/`esp_netif`; secrets/creds in NVS. SoftAP captive portal is the recovery UX.
```c
// Primary: try each saved network, early-abort on auth-fail/no-AP, per-net timeout ~15s.
esp_err_t net_connect_saved(uint32_t per_net_timeout_ms);   // -> ESP_OK on first success
bool      net_is_connected(void);
void      net_start_auto_reconnect(void);                   // backoff on mid-session drop
// NVS credential list
esp_err_t net_creds_add(const char *ssid, const char *pass);
esp_err_t net_creds_clear(void);
// Fallback: SoftAP "AMOLED-setup" + esp_http_server form + DNS catch-all -> save -> reconnect.
esp_err_t net_portal_start(const char *ap_ssid);            // P0.5
void      net_portal_stop(void);
```

## 6. AG-UI integration

### Event → UI (v1 subset)
| Event | Handling |
|---|---|
| `RUN_STARTED` | status: "thinking" |
| `TEXT_MESSAGE_START/CONTENT/END` | chat: assistant bubble (stream + finalize) |
| `REASONING_*` | ephemeral "reasoning…" (clears when text starts); ignore `REASONING_ENCRYPTED_VALUE` |
| `TOOL_CALL_START/ARGS/END/RESULT` | ephemeral chip; if a **client** tool → execute → `agui_tool_result` |
| `RUN_FINISHED` (outcome=interrupt) | interrupt prompt → resume |
| `RUN_FINISHED` / `RUN_ERROR` | finalize / show error |
| `STATE_*` · `ACTIVITY_*` · `MESSAGES_SNAPSHOT` | **deferred to v2** (needs RFC 6902 JSON-Patch) |

### Ambient context (push every run → `RunAgentInput.context`, read-only)
```json
[ {"description":"device_motion","value":{"activity":"walking","wrist_raised":true,"steps_today":4120,"orientation":"face_up"}},
  {"description":"battery","value":{"pct":72,"charging":false}},
  {"description":"local_time","value":"2026-06-21T14:03:11-04:00"} ]
```

### Interrupt / HITL
- Detect `RUN_FINISHED.outcome == "interrupt"`; read each `interrupts[]` entry (`id`, `message`, `response_schema`, `expires_at`).
- Render by schema: boolean → Yes/No; enum → tappable list; string → on-screen input or **QR-to-phone** handoff.
- Resume: new `RunAgentInput` with `resume:[{interruptId, value}]`, same `threadId`. **Align the resume carrier** (`resume` array vs `forwardedProps.resume`) with the user's agent (`home-agui-agent` / LangGraph). Honor `expires_at` (timeout → cancel).

### Client-side tools (v1)
| Tool | Args | Effect | UI |
|---|---|---|---|
| `set_timer` | `{seconds,label}` | start countdown | ambient idle countdown |
| `set_alarm` | `{iso_time,label}` | schedule on RTC | none until it fires |
| `show_qr` | `{data}` | render QR | `lv_qrcode` |

Advertised in `RunAgentInput.tools`; on `TOOL_CALL_*` the device executes locally and returns `TOOL_CALL_RESULT`.

## 7. Soniox integration
- **Mic capture** (ref `app-pixels/ai-chat` + `ai-assistant-claude` `audio_engine.cpp`, both MIT):
  16 kHz **mono s16le**, MCLK 4.096 MHz (256×), mic gain ~5, I2S std **stereo capture → mono
  downmix**, lock-free ring buffer ~32 k samples (~2 s). We use the BSP `esp_codec_dev` mic path;
  confirm pins (BCLK=9, MCLK=16, WS=45, DOUT=8, DIN=10).
- First text frame = config (`api_key`, `model:"stt-rt-v5"`, `audio_format`, `sample_rate:16000`, `num_channels:1`, `enable_endpoint_detection:true`); then **binary** PCM frames; **empty frame** to end.
- Parse `tokens[]` → accumulate `is_final` text; final message has `"finished":true`.
- **Verify at build time** the exact `audio_format` value for raw PCM (e.g. `pcm_s16le`) and sample-rate handling against current Soniox docs.

## 8. C++ AG-UI binding port
- Source: `ag-ui-protocol/ag-ui` → `sdks/community/c++` — a *full* client (27 events, SSE parser, RFC 6902 state) but **desktop-oriented** (libcurl + nlohmann/json + cmake/pkg-config).
- Port: libcurl → `esp_http_client` (streaming) + esp-tls · nlohmann/json → `cJSON` · drop exceptions/RTTI · buffers in PSRAM.
- **Three device extensions** (net-new atop the port): (1) inject `context` per run; (2) Interrupt → render-by-schema → resume; (3) client-side tool execution.
- **v1 can skip `STATE_*` + JSON-Patch** (chat + ephemeral status only) — removes the hardest piece (RFC 6902) from v1; add in v2 for shared-state-driven UI.
- Check the community SDK's license before lifting code.

## 9. UI design (LVGL 8.4)
- **Chat:** scrollable flex column of bubbles; assistant bubble updates per delta + autoscroll.
  (UI cues borrowed from `app-pixels/ai-assistant-claude`: color-coded status pill, a listening
  VU meter, "You:/AI:" labels, half-page scroll, ~4 KB history pruning.)
- **Ephemeral status:** status `lv_label` + spinner; tool chips as transient toasts.
- **Interrupt prompt:** modal overlay; widgets from `response_schema` (`lv_btnmatrix`/`lv_msgbox` for choices/yes-no, `lv_qrcode` for string handoff).
- **Idle:** clock + active timer countdown.
- **Font management:** decide glyph coverage up front — Latin-1 + punctuation for v1 (defer
  CJK/emoji; they balloon flash). Embed `lv_font_t`(s) in flash; set a **fallback font/glyph** so
  missing codepoints don't render as tofu. (LVGL handles UTF-8 + clipping natively — unlike the
  `Arduino_GFX` CP437 transliteration in `app-pixels/*`, which we do **not** port.)
- Inset content for the ~50 px corner radius.

## 10. Build milestones
| Phase | Goal | Acceptance |
|---|---|---|
| **P0** Environment + WiFi | IDF 5.5 pinned; flash workflow proven (`docs/flashing.md`); `net_prov` NVS multi-SSID connect + auto-reconnect | `05_LVGL` builds + flashes (display+touch); device joins WiFi from NVS creds |
| **P0.5** Provisioning | `net_prov` SoftAP captive portal (`AMOLED-setup`) + web form + DNS catch-all | no NVS creds → AP appears → submit from phone → saved to NVS → connects |
| **P1** Soniox STT | `soniox_client`: ES8311 capture (ref `app-pixels/ai-chat` `audio_engine.cpp`) → WS → transcript | speak → partial+final transcript on serial |
| **P2** AG-UI text | `agui_client` core (RunAgentInput + SSE + TEXT_*) | transcript → agent → reply text (serial) |
| **P3** Chat UI | `chat_ui` chat list + streaming | conversation renders on AMOLED |
| **P4** Status | REASONING_*/TOOL_CALL_*/RUN_* → status/toast | live activity shows + clears |
| **P5** Context | ambient motion+battery+time → `context` | agent receives device context each run |
| **P6** Interrupt | detect → render-by-schema → resume; `show_qr` | a HITL question is answered on-screen |
| **P7** Tools | `set_timer` (idle countdown) + `set_alarm` (RTC) | agent sets a timer/alarm; result round-trips |
| **P8** Hardening | NVS secrets + Soniox ephemeral keys, reconnect/backoff, PSRAM tuning, error states; then v2 tools | survives drops; no key on device |

**Deferred / backlog (post-P8, "if we get to it"):**
- **A2UI generative-UI rendering.** Observed 2026-06-21: the user's `home-agui-agent` drives the
  screen via a `render_a2ui` TOOL_CALL carrying an [A2UI](https://a2ui.org) component tree
  (e.g. `Column`>`Text` from `…/v0_9/basic_catalog.json`) and finishes the run with **no**
  `TEXT_MESSAGE_CONTENT` — the reply text lives inside the A2UI spec. Rendering A2UI trees in LVGL is
  a substantial UI layer. **Parked past v1**; P3–P8 target a `TEXT_*`-emitting agent. The agui_client
  event router is left as-is (it already surfaces the tool call). Revisit if on-device A2UI proves worth it.
- **Wake-word / PTT turn control (post-P8).** Open the Soniox session only for an *active turn*
  instead of streaming continuously. This is also the **correct** fix for streaming idle silence —
  NOT on-device chunk VAD, which is incompatible with Soniox (it does its own server VAD/endpointing
  and times out the session after ~20 s of no audio; tried & reverted 2026-06-22). Two flavors with
  very different power:
  - **PTT — CHOSEN (2026-06-22).** Repurpose the **BOOT button (GPIO0)** as a hold-to-talk key:
    press → open Soniox + stream; release → `soniox_finalize` → transcribe → run the agent. GPIO0 is
    only a strapping pin at *reset* (don't hold during power-on); at runtime it's a normal input.
    The session is open ONLY while held → no continuous streaming → structurally avoids the Soniox
    idle-timeout + idle-garbage (no VAD needed), and behaves identically plugged or on battery.
    Use case: **desktop-primary, battery-portable secondary** — PTT fits both. Device can sleep
    between turns → days of standby on battery.
  - **Always-listening wake-word** (ESP-SR WakeNet on core 1): CPU + mic + WiFi never sleep →
    rough **~4–8 h on a small ~500 mAh cell** (scale by actual battery; display-off + WiFi DTIM
    power-save pushes toward the high end). Needs a WakeNet **model partition** (partition-table
    change + flash). Add only if hands-free justifies the battery hit; measure real draw via the
    AXP2101 before committing.
  - **Note:** wake-word *detection* is fully on-device (WakeNet, no network). WiFi only matters
    *after* a wake (to reach Soniox); it can be dropped during idle-listen and reconnected on wake
    (lower power, +reconnect latency) rather than held associated.
- **Soniox recognition-accuracy tuning (post-P8).** Domain context/hints so proper nouns/terms
  aren't mangled, and possibly a higher-accuracy Soniox model. These are Soniox-side levers, not
  on-device — independent of the capture path.

## 11. Risks / open questions
- **Heap headroom** for one TLS session + LVGL + audio buffers — measure peak; keep buffers in PSRAM. (Sequential TLS avoids two concurrent sessions.)
- **Soniox raw-PCM format** (`audio_format`/sample-rate field) — confirm against live docs.
- **Resume carrier** (`resume` array vs `forwardedProps.resume`) — match the user's AG-UI agent.
- **C++ SDK reuse** — confirm its SSE/event code transcribes cleanly to cJSON; check license.
- **JSON-Patch** (RFC 6902) correctness — deferred to v2; test `move`/`copy`/`test` ops when added.
- **Wake:** v1 = PTT (BOOT/touch); optional ESP-SR WakeNet later (on-device wake word).

## 12. References
- AG-UI C++ SDK (port source): `ag-ui-protocol/ag-ui` → `sdks/community/c++`
- AG-UI agent side (user's bridge / reference): `contextablemark/home-agui-agent`
- Reference apps (this exact board, **MIT**, Arduino-core → **pattern-level** reuse, not copied):
  - `app-pixels/ai-chat` — ES8311 capture + WiFi multi-SSID fallback (`audio_engine.cpp`, `app_common.cpp`)
  - `app-pixels/ai-assistant-claude` — font handling + chat/status UI + agentic tool loop
- Soniox real-time WebSocket API: `soniox.com/docs/stt`
- Board base: `examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM` and `06_I2SCodec`
- Flashing (USB-C `idf.py flash` + NVS-wipe recovery): `docs/flashing.md`
- Use `/idf-docs` for DeepWiki lookups against these repos.
