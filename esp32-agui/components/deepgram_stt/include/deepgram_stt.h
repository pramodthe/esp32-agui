// deepgram_stt — live STT via Deepgram Listen v1 WebSocket.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *endpoint;    // NULL -> wss://api.deepgram.com/v1/listen?...
    const char *api_key;     // required (facade supplies from NVS)
    const char *model;       // NULL -> "nova-3"
    int         sample_rate; // 0 -> 16000
} deepgram_stt_cfg_t;

typedef void (*deepgram_stt_partial_cb)(const char *running_text, void *ctx);
typedef void (*deepgram_stt_turn_cb)(const char *final_text, void *ctx);

esp_err_t deepgram_stt_init(void);
// Release the mic codec handle so another STT backend can own I2S-RX (provider switch).
void deepgram_stt_deinit(void);
esp_err_t deepgram_stt_mic_stop(void);
esp_err_t deepgram_stt_mic_start(void);

esp_err_t deepgram_stt_session_start(const deepgram_stt_cfg_t *cfg,
                                     deepgram_stt_partial_cb on_partial,
                                     deepgram_stt_turn_cb on_turn, void *ctx);

esp_err_t deepgram_stt_session_finalize(void);
void deepgram_stt_session_stop(void);
bool deepgram_stt_session_active(void);
const char *deepgram_stt_last_error(void);

#ifdef __cplusplus
}
#endif
