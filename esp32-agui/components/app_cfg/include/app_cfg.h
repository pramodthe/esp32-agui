// app_cfg — tiny NVS-backed string config/secret store (namespace "appcfg").
// Written by the captive portal (net_prov), read by soniox_client / agui_client.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CFG_SONIOX_KEY "soniox_key"   // Soniox API key (permanent; ephemeral mint is P8)
#define APP_CFG_AGUI_URL   "agui_url"     // AG-UI endpoint (P2)
#define APP_CFG_AGUI_TOKEN "agui_token"   // AG-UI bearer (P2)
#define APP_CFG_TZ         "tz"           // POSIX TZ string for local_time context (P5; default UTC0)
#define APP_CFG_TTS_VOICE  "tts_voice"    // Soniox TTS voice name (P8; default "Adrian")
#define APP_CFG_TTS_VOL    "tts_vol"      // spoken-reply volume 0-100 (volume buttons; default 90)
#define APP_CFG_SCREEN_TO  "scr_to"       // screen blank timeout, seconds (default 60; 0 = always on)
#define APP_CFG_IDLE_ANIM  "idle_anim"    // idle screensaver flag "0"/"1" (default 0; pulses alarm image)

// Max stored value length (incl. NUL). One number shared by the portal form, the NVS
// writer, and every reader so a long value can't pass provisioning then fail to load.
#define APP_CFG_VAL_MAX 192

// Store a string value under key. Empty/NULL val clears the key.
esp_err_t app_cfg_set(const char *key, const char *val);

// Read key into buf (NUL-terminated). Returns false if missing or empty.
bool app_cfg_get(const char *key, char *buf, size_t len);

// True if key exists and is non-empty.
bool app_cfg_has(const char *key);

#ifdef __cplusplus
}
#endif
