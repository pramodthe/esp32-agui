// soniox_client — real-time streaming STT over WebSocket (Soniox).
//
// Mic (ES8311 via BSP esp_codec_dev, 16 kHz mono s16le) -> WSS to Soniox -> live transcript.
// Protocol verified in docs/soniox-rt-protocol.md (endpoint, config frame, pcm_s16le,
// stt-rt-v5, token/<end>/finished shape). API key comes from NVS (app_cfg) by default.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *endpoint;   // NULL -> wss://stt-rt.soniox.com/transcribe-websocket
    const char *api_key;    // NULL -> read APP_CFG_SONIOX_KEY from NVS
    const char *model;      // NULL -> "stt-rt-v5"
    int         sample_rate;// 0   -> 16000
} soniox_cfg_t;

// Callbacks fire from the websocket task and MUST be fast and non-blocking, and MUST NOT
// call soniox_session_finalize()/stop() inline (route those to another task).
//
// Live transcript update: running = committed final text + current interim tail.
typedef void (*soniox_partial_cb)(const char *running_text, void *ctx);
// Utterance boundary (Soniox "<end>" token): committed text for the finished turn.
typedef void (*soniox_turn_cb)(const char *final_text, void *ctx);

// Bring up the microphone codec (16 kHz mono). Call once.
esp_err_t soniox_client_init(void);

// Idle low-power: stop/restart the mic codec (disables/enables the I2S so it releases its
// NO_LIGHT_SLEEP lock, letting the CPU light-sleep when idle). mic_start() must precede a session.
esp_err_t soniox_client_mic_stop(void);
esp_err_t soniox_client_mic_start(void);

// Open the WSS, send the config frame, and start streaming mic audio. Callbacks fire
// from the websocket task. Returns once the session is starting (non-blocking).
esp_err_t soniox_session_start(const soniox_cfg_t *cfg,
                               soniox_partial_cb on_partial,
                               soniox_turn_cb on_turn, void *ctx);

// Flush the current utterance to final without closing (PTT/turn-taking).
esp_err_t soniox_session_finalize(void);

// Stop streaming, send end-of-audio, close the socket.
void soniox_session_stop(void);

// True while a session is running AND healthy (false after a fatal Soniox error).
bool soniox_session_active(void);

// Last fatal error reported by Soniox (e.g. bad key), or NULL if none.
const char *soniox_last_error(void);

#ifdef __cplusplus
}
#endif
