// speech_tts — facade over Deepgram / Soniox streaming TTS (provider from NVS).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TTS_PCM_SINK_T_DEFINED
#define TTS_PCM_SINK_T_DEFINED
typedef void (*tts_pcm_sink_t)(const void *pcm, size_t bytes);
#endif

esp_err_t speech_tts_init(tts_pcm_sink_t sink);
esp_err_t speech_tts_speak(const char *text);
esp_err_t speech_tts_open(void);
esp_err_t speech_tts_feed(const char *text);
esp_err_t speech_tts_finish(void);
esp_err_t speech_tts_wait_drained(uint32_t timeout_ms);
void speech_tts_cancel(void);

#ifdef __cplusplus
}
#endif
