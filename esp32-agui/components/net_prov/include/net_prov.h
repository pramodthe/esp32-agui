// net_prov — WiFi connectivity + provisioning.
//
// Primary: multi-SSID list stored in NVS, tried with early-abort (pattern adapted from
// app-pixels/ai-chat wifi_try_connect(), ported to esp_wifi/esp_netif).
// Fallback (P0.5): SoftAP "AMOLED-setup" captive portal to enter creds from a phone.
// See docs/esp32-agui-plan.md §5.5.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time init (NVS netif/event loop wiring). Safe to call once from app_main.
esp_err_t net_prov_init(void);

// Primary connect: try each saved network in turn, early-abort on auth-fail / no-AP,
// per-network timeout. Returns ESP_OK on first successful association.
esp_err_t net_connect_saved(uint32_t per_net_timeout_ms);

bool net_is_connected(void);

// Format the current STA IPv4 into buf (e.g. "192.168.1.42"). Returns false if offline.
bool net_get_ip_str(char *buf, size_t len);

// Start auto-reconnect with backoff for mid-session drops.
void net_start_auto_reconnect(void);

// Start SNTP (one-shot) so the system clock holds real wall-clock time for ambient context.
// Call once after WiFi is up; it syncs in the background.
void net_sntp_start(void);

// True once the clock has been set (by SNTP or the HTTPS-Date fallback).
bool net_time_synced(void);

// Fallback time source for networks that block NTP: read an HTTPS server's Date header and set the
// clock. No-op once already synced. Safe to call repeatedly (e.g. from a heartbeat) until it succeeds.
esp_err_t net_time_http_fallback(void);

// Toggle WiFi modem-sleep. true = WIFI_PS_NONE (low latency, higher power) for the duration of a
// turn (mic streaming + agent reply); false = WIFI_PS_MIN_MODEM (default, power-saving) when idle.
// Default modem-sleep adds ~100ms/round-trip which throttles the Soniox upload — see soniox_client.
void net_low_latency(bool on);

// Idle low-power: net_wifi_suspend() powers the WiFi radio fully off (disconnect + stop, auto-reconnect
// disabled); net_wifi_resume() powers it back on and re-associates (async — poll net_is_connected()).
// Used to shed WiFi when idle (display off + on battery). Resume is only valid after a suspend.
void net_wifi_suspend(void);
void net_wifi_resume(void);

// NVS credential list management.
esp_err_t net_creds_add(const char *ssid, const char *pass);
esp_err_t net_creds_clear(void);

// Fallback provisioning (P0.5): SoftAP + captive-portal web form + DNS catch-all.
esp_err_t net_portal_start(const char *ap_ssid);
void      net_portal_stop(void);
// Block until the user submits credentials via the portal (saved to NVS), or timeout.
// Returns true if credentials were saved.
bool      net_portal_wait_saved(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
