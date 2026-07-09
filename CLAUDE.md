# ESP32-S3-Touch-AMOLED-1.8 — Project Guide

This repo (**`contextablemark/esp32-agui`**) is the custom **on-device AG-UI voice client**
firmware for the **Waveshare ESP32-S3-Touch-AMOLED-1.8** board. The ESP-IDF project lives in
[esp32-agui/](esp32-agui/) and is **self-contained** (it vendors the board BSP + drivers under
`esp32-agui/components/`).

> The board's stock `examples/` and factory `Firmware/` `.bin` images referenced below are **not
> in this repo** — they live in the upstream Waveshare repo
> (`github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8`). Treat such references as pointers there.

## Hardware

- **MCU:** ESP32-S3 (Xtensa LX7 dual-core). Octal (OPI) PSRAM @ 80 MHz, 16 MB flash.
- **Display:** 1.8″ AMOLED, **368 × 448 px**, QSPI interface.
  (The repo `README.md` says "368×488" — that's a typo; the driver code uses
  `H_RES=368, V_RES=448`.)
- **Two hardware revisions**, auto-detected at runtime by `board_variant_detect()`
  (probes the touch controller over I²C — see
  [board_variant.c](examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM/components/board_variant/board_variant.c)):
  - **V1 "original" — SH8601 display + FT3168 touch (I²C 0x38)  ← this board**
  - V2 "modified" — CO5300 display + CST816 touch (I²C 0x15)
  - One binary serves both; nothing to fork. With no panel connected, detection
    logs `UNKNOWN` (a runtime state, not a build error).
- **Other peripherals:** AXP2101 PMIC, PCF85063 RTC, QMI8658 6-axis IMU,
  ES8311 audio codec + dual digital mics, SD/MMC slot, 3.7 V Li battery (MX1.25).
- **Key pins (V1, from the LVGL example):** LCD QSPI on `SPI2_HOST` —
  CS=12, PCLK=11, D0=4, D1=5, D2=6, D3=7. Touch I²C — SDA=15, SCL=14, INT=21.
  An I/O expander at `0x20` drives LCD_RST / TOUCH_RST / SD_CS.

## ⚠️ Toolchain: ESP-IDF 5.5.x ONLY

The examples target **ESP-IDF 5.5.x**, and their managed components cap at `idf <6.0`
(e.g. `espressif/es8311` requires `>=4.4,<6.0`). **ESP-IDF 6.x breaks dependency
solving** with:

```
ERROR: Version solving failed: no versions of idf match >=5.5.0,<6.0.0
```

If you see that, you're on the wrong IDF. Install ESP-IDF **5.5.x** (e.g.
`git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf &&
~/esp/esp-idf/install.sh esp32s3`, then `. ~/esp/esp-idf/export.sh`). Target chip is always
**esp32s3**.

## Layout

- `examples/ESP-IDF-v5.5.1/` — ESP-IDF examples (build these):
  | Dir | What |
  |---|---|
  | `01_AXP2101` | PMIC / power management |
  | `02_PCF85063` | RTC |
  | `03_QMI8658` | 6-axis IMU |
  | `04_SD_MMC` | SD card storage |
  | **`05_LVGL_WITH_RAM`** | **display + touch + LVGL 8.4 — start here for UI** |
  | `06_I2SCodec` | ES8311 audio in/out |
- `examples/Arduino-v3.3.5/`, `examples/Arduino-v3.3.5-v2/` — Arduino-core variants.
- `Firmware/` — prebuilt factory ("Xiaozhi") `.bin` images (16 MB); flash directly
  to restore stock firmware.
- Each example **vendors its managed components** under `<example>/components/`
  (`esp_lcd_sh8601`, `esp_lcd_co5300`, `esp_lcd_touch_*`, `lvgl`, …) — prefer reading
  those for the exact in-use versions.

## Build / flash / run

Run from inside an example dir, e.g. `examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM`
(source the IDF env first: `. ~/esp/esp-idf/export.sh`).

- **Build:** `idf.py build` — target esp32s3 comes from `sdkconfig.defaults`; don't run
  `set-target` (it wipes `sdkconfig`).
- **Flash + monitor (real HW):** `idf.py -p <PORT> flash monitor` — `<PORT>` e.g.
  `/dev/ttyACM0` (Linux), `/dev/cu.usbmodem*` (macOS), or `COMx` (Windows), over the
  board's USB-C (native USB Serial/JTAG).
- **QEMU:** `idf.py qemu` — **CPU/RAM/UART only.** The AMOLED, touch, I²C sensors, SD,
  and codec are **not emulated**, so it's for boot/app-logic/networking, not the UI.
- **Config / clean:** `idf.py menuconfig` · `idf.py fullclean`.

Slash-commands in `.claude/commands/` wrap these: `/idf-build` `/idf-flash`
`/idf-monitor` `/idf-qemu` `/idf-size` `/idf-docs`.

## Driver / API docs (agentic lookup)

The **DeepWiki MCP** is connected — ask driver questions against the upstream repos
(or just run `/idf-docs <question>`):

- `espressif/esp-iot-solution` — SH8601 & CO5300 AMOLED drivers, `esp_lcd_panel_io_additions`, `cmake_utilities`
- `espressif/esp-bsp` — `esp_lcd_touch`, FT5x06/FT3168 & CST816S touch, ES8311 codec
- `lvgl/lvgl` — LVGL 8.4 API
- `espressif/esp-idf` — core `esp_lcd` / I²C / SPI / I²S / SD-MMC peripheral APIs

For broad multi-source research (datasheets + several repos), use the `deep-research` skill.

> Example (SH8601 QSPI init, via DeepWiki): panel I/O uses `dc_gpio_num=-1`,
> `lcd_cmd_bits=32`, `lcd_param_bits=8`, `flags.quad_mode=true`; vendor config sets
> `flags.use_qspi_interface=1`; set brightness with command `0x51`
> (`esp_lcd_panel_io_tx_param(io, 0x51, &level, 1)`).

## Roadmap / planned architecture: on-device AG-UI voice client

**Decision (committed).** Custom **ESP-IDF** firmware that makes the device a *first-class
AG-UI client* — not a thin voice satellite. It streams mic audio to **Soniox** (real-time STT),
is itself the **AG-UI** client (POST `RunAgentInput` → consume SSE), and renders chat + live
agent activity on the AMOLED via **LVGL**. Putting the AG-UI client on-device is the point:
`TOOL_CALL_*`/interrupt events drive the screen, and the device exposes its sensors/screen/touch
as agent-callable tools + ambient context.

→ **Full plan (components, APIs, state machine, build phases): [docs/esp32-agui-plan.md](docs/esp32-agui-plan.md).**

**Base:** ESP-IDF 5.5.x on this repo's BSP (`esp32_s3_touch_amoled_1_8`, from
`examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM` + `06_I2SCodec`) — SH8601 + FT3168 + ES8311 + LVGL 8.4
already wired. `app-pixels/ai-chat` (Arduino) is a *reference* for ES8311 capture only
(Arduino_GFX ≪ LVGL here). Not chosen but viable: **Xiaozhi** (factory fw, board officially
supported) and **ESPHome+HA** (reuses the user's `home-agui-agent` HA→AG-UI bridge) — the
thin-device routes; comparison in the plan doc.

**Pipeline**
```
mic → ES8311/I²S (16k s16le) → Soniox WSS (streaming) → transcript
  → AG-UI: POST RunAgentInput{ messages, context:[motion,battery,time] } → SSE
  → LVGL: chat (TEXT_*) · ephemeral status (REASONING_*/TOOL_CALL_*/RUN_*) · interrupt prompt
  → client tools (set_timer/set_alarm/show_qr) run on-device → TOOL_CALL_RESULT
  → RUN_FINISHED outcome=interrupt → render by response_schema → resume (new run)
```
No barge-in (single ES8311, no echo reference) → wake/PTT turn-taking.

**Components (ESP-IDF):** `soniox_client/` (STT over `esp_websocket_client`) · `soniox_tts_client/`
(streaming spoken replies + PTT barge-in) · `agui_sdk/` + `agui_client/` (AG-UI client — **vendors +
ESP-ports `ag-ui-protocol/ag-ui/sdks/community/c++`**: libcurl→`esp_http_client`, nlohmann→cJSON;
+device extensions — per-run `context`, REASONING_*, Interrupt→resume, client-tool dispatch;
`agui_client` is a thin `extern "C"` shim) · `device_tools/` (tools + ambient context from
QMI8658/AXP2101/PCF85063) · `net_prov/` (WiFi + SoftAP captive portal) · `app_cfg/` (NVS config) ·
`alarm_img/` (user-uploaded alarm graphic in a flash partition) · `chat_ui/` (LVGL chat + status +
configurable screen saver + ringing-alarm overlay + idle screensaver of the uploaded image).

**v1 surface:** ambient `context` (motion+battery+time, read-only) · Interrupt/HITL
(`response_schema`-driven touch prompt, resume) · tools `set_timer` (idle countdown) /
`set_alarm` (silent) / `show_qr` (`lv_qrcode`). **v2:** STATE_*/JSON-Patch, `display_card`/
`render_chart`/`show_image`, `ble_*`, `sd_*`, `set_brightness`/`notify`.

**Constraints:** ESP-IDF **5.5.x only**; secrets (Soniox key / AG-UI URL+token) in **NVS**,
prefer Soniox **ephemeral keys** (`POST /v1/auth/temporary-api-key`); TLS run **sequentially**
(listen → respond), buffers in **PSRAM**.
