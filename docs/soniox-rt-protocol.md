# Soniox real-time STT — verified protocol (for `soniox_client`)

Verified 2026-06-21 against live Soniox docs + the official `soniox_realtime.py` example
(multi-source, adversarially checked; confidence: high). Drives [esp32-agui/components/soniox_client](../esp32-agui/components/soniox_client/).

## Endpoint & auth
- **WSS:** `wss://stt-rt.soniox.com/transcribe-websocket` (TLS; no query/subprotocol).
- **Auth is in-band:** the API key is the top-level `api_key` field of the **first** message
  (a JSON **text** frame). The WS upgrade itself is unauthenticated.
- **v1 (chosen):** use the permanent key from NVS directly as `api_key`.
- **Ephemeral (P8 hardening):** `POST https://api.soniox.com/v1/auth/temporary-api-key`,
  `Authorization: Bearer <permanent key>`, body `{usage_type:"transcribe_websocket",
  expires_in_seconds:1..3600}` → `201 {"api_key":"temp:...","expires_at":"<UTC ISO>"}`. Use the
  `temp:` string as `api_key`. Mint over a **separate** HTTPS/TLS session **before** opening WSS
  (sequential-TLS rule).

## First frame (text) — config
```json
{"api_key":"<key>","model":"stt-rt-v5","audio_format":"pcm_s16le",
 "sample_rate":16000,"num_channels":1,"language_hints":["en"],
 "enable_endpoint_detection":true}
```
- **`audio_format` = `pcm_s16le`** (canonical — do NOT use `s16le`; that was the single
  highest-risk wrong value seen). **`model` = `stt-rt-v5`** (v4 removed 2026-06-30).

## Audio + control
- After config, stream raw PCM as **binary** WS frames (s16le bytes, no base64, no WAV header) —
  `esp_websocket_client_send_bin()`. Natural chunk ≈ **3840 bytes = 1920 samples = 120 ms** at
  16 k/mono/s16le. One task owns the socket (send is not concurrent-safe).
- **End of audio:** send a single **empty** frame (`send_text("",0)` or `send_bin len 0`) → server
  emits a `finished` response then closes.
- **Control text frames:** `{"type":"finalize"}` flushes the current utterance to final **without
  closing** (use for PTT/turn-taking); `{"type":"keepalive"}` during silence if not streaming.
  (Transport WS ping/pong is handled by `esp_websocket_client` ping_interval — complementary.)

## Server responses (text JSON)
```json
{"tokens":[{"text":"Hello","start_ms":600,"end_ms":760,"confidence":0.97,"is_final":true}],
 "final_audio_proc_ms":760,"total_audio_proc_ms":880}
```
- **Finality model:** `is_final:false` tokens are the live interim tail — **replace** it each
  message. `is_final:true` tokens are **committed** — append, never retracted.
  Running transcript = committed + current non-final tail.
- **Turn boundary:** with endpoint detection, a final token with **`text == "<end>"`** marks the
  utterance end → take committed transcript, (later) fire the AG-UI run. Be tolerant: if `<end>`
  is absent, fall back to last-final-settle + silence timeout.
- **Session end:** `{"tokens":[],...,"finished":true}` (only after the empty frame), then close.
- **Errors:** `{error_code,error_type,error_message,...}`.

## ESP-IDF client notes
- `espressif/esp_websocket_client` **^1.7.0** (IDF ≥5.0; OK on 5.5.x).
- WSS cert verify: `crt_bundle_attach = esp_crt_bundle_attach` +
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` / `..._DEFAULT_FULL=y`.
- Client TX/RX buffers are **internal DRAM** (buffer_size sets size, not location). Keep
  `buffer_size` modest (2–4 KB), rely on multi-event reassembly, and put **your reassembly
  buffer in PSRAM** (`heap_caps_malloc(payload_len+1, MALLOC_CAP_SPIRAM)`).
- **Reassembly:** a message > buffer_size arrives as multiple `WEBSOCKET_EVENT_DATA` events —
  first has the real `op_code` (0x01) and `payload_offset==0`; later are `op_code 0x00` (CONT);
  complete when `fin && payload_offset+data_len==payload_len`. Then cJSON-parse.

Sources: soniox.com/docs (real-time, websocket-api, models, endpoint-detection, audio-formats,
auth), soniox/soniox_examples `soniox_realtime.py`, components.espressif.com esp_websocket_client.
