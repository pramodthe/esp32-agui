// deepgram_tts — spoken replies via Deepgram Speak v1 WebSocket (linear16 @ 16 kHz).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t deepgram_tts_init(void (*sink)(const void *pcm, size_t bytes));
void deepgram_tts_deinit(void);
esp_err_t deepgram_tts_speak(const char *text);
esp_err_t deepgram_tts_open(void);
esp_err_t deepgram_tts_feed(const char *text);
esp_err_t deepgram_tts_finish(void);
esp_err_t deepgram_tts_wait_drained(uint32_t timeout_ms);
void deepgram_tts_cancel(void);

#ifdef __cplusplus
}
#endif
