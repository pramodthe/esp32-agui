// speech_cfg — resolve speech provider + API key from NVS (portal / migration).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPEECH_PROVIDER_SONIOX   = 0,
    SPEECH_PROVIDER_DEEPGRAM = 1,
} speech_provider_t;

#define SPEECH_PROVIDER_DEEPGRAM_STR "deepgram"
#define SPEECH_PROVIDER_SONIOX_STR   "soniox"

// Cached after first NVS read. Call speech_cfg_invalidate() after portal saves.
speech_provider_t speech_provider_get(void);

const char *speech_provider_name(speech_provider_t p);

// Drop the in-memory provider cache so the next get() re-reads NVS.
void speech_cfg_invalidate(void);

// Load the active speech API key. Prefer APP_CFG_SPEECH_KEY; for Soniox also fall back to
// legacy APP_CFG_SONIOX_KEY.
bool speech_cfg_get_key(char *buf, size_t len);

// True if a usable speech API key is configured for the active provider.
bool speech_cfg_has_key(void);

#ifdef __cplusplus
}
#endif
