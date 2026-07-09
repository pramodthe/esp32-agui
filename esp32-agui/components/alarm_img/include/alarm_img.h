// alarm_img — persistent, user-uploaded alarm graphic stored in a dedicated flash partition.
//
// The captive portal (net_prov/portal.c) converts any uploaded image to raw RGB565 in the browser
// and POSTs exactly ALARM_IMG_BYTES; alarm_img_write() validates + stores it. chat_ui loads it at
// alarm time and shows it as an lv_img (centered, on black), pulsing with the beeps. No image stored
// (fresh/erased partition) → chat_ui falls back to the built-in red ring.
//
// Pixel byte order matches LVGL's LV_COLOR_16_SWAP (the device renders RGB565); the browser emits
// that order so the device stores the bytes verbatim — no on-device decode or conversion.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_IMG_W     240
#define ALARM_IMG_H     240
#define ALARM_IMG_BYTES (ALARM_IMG_W * ALARM_IMG_H * 2)   // RGB565 → 115200 bytes

// Validate (len must equal ALARM_IMG_BYTES) and persist a freshly-uploaded image: erase the
// partition, write a small header + the pixels. Returns ESP_ERR_INVALID_SIZE on a length mismatch,
// ESP_ERR_NOT_FOUND if the partition is absent (old table flashed), or a flash error.
esp_err_t alarm_img_write(const uint8_t *data, size_t len);

// True iff a valid image is stored (header magic + dimensions + size all match).
bool alarm_img_present(void);

// Load the stored image into a freshly-allocated PSRAM buffer (ALARM_IMG_BYTES of RGB565). The
// caller owns it and must release it with alarm_img_free(). Returns ESP_ERR_NOT_FOUND if none stored.
esp_err_t alarm_img_load(uint8_t **out);
void      alarm_img_free(uint8_t *buf);

// Erase the stored image (revert to the default ring). No-op if none stored.
esp_err_t alarm_img_clear(void);

#ifdef __cplusplus
}
#endif
