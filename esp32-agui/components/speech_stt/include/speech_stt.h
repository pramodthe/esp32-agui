// speech_stt — facade over Deepgram / Soniox streaming STT (provider from NVS).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *endpoint;   // optional provider override
    const char *api_key;    // NULL -> speech_cfg_get_key()
    const char *model;      // optional
    int         sample_rate;// 0 -> 16000
} speech_stt_cfg_t;

typedef void (*speech_stt_partial_cb)(const char *running_text, void *ctx);
typedef void (*speech_stt_turn_cb)(const char *final_text, void *ctx);

esp_err_t speech_stt_init(void);
esp_err_t speech_stt_mic_stop(void);
esp_err_t speech_stt_mic_start(void);

esp_err_t speech_stt_session_start(const speech_stt_cfg_t *cfg,
                                   speech_stt_partial_cb on_partial,
                                   speech_stt_turn_cb on_turn, void *ctx);

esp_err_t speech_stt_session_finalize(void);
void speech_stt_session_stop(void);
bool speech_stt_session_active(void);
const char *speech_stt_last_error(void);

#ifdef __cplusplus
}
#endif
