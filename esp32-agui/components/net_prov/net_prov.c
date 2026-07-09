// net_prov core — WiFi STA connect with multi-SSID early-abort + auto-reconnect.
// Pattern adapted from app-pixels/ai-chat wifi_try_connect(), ported to esp_wifi/esp_netif.
// Captive-portal fallback lives in portal.c (P0.5).

#include "net_prov.h"
#include "net_prov_internal.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"

static const char *TAG = "net_prov";

// struct tm (interpreted as UTC) -> time_t, without timegm (absent from ESP-IDF newlib) and without
// mutating the global TZ. Howard Hinnant's days_from_civil. Valid for the dates we'll ever see.
static time_t tm_to_utc(const struct tm *tm)
{
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

#define NVS_NS         "netprov"
#define KEY_COUNT      "count"
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

#define BIT_CONNECTED  BIT0   // got IP
#define BIT_FAIL       BIT1   // disconnected while in initial-connect loop

static EventGroupHandle_t s_eg;
static esp_netif_t       *s_sta_netif;
static esp_timer_handle_t s_reconnect_timer;
static volatile bool      s_connecting;   // inside net_connect_saved() attempt
static volatile bool      s_auto;         // auto-reconnect enabled
static volatile bool      s_online;       // currently has IP
static esp_ip4_addr_t     s_ip;           // current STA IPv4 (valid when s_online)
static uint32_t           s_backoff_ms = BACKOFF_MIN_MS;

// ---- NVS credential list -------------------------------------------------

int net_creds_load(net_cred_t *out, int max)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t count = 0;
    nvs_get_u8(h, KEY_COUNT, &count);
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        char ks[16], kp[16];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        size_t ls = sizeof(out[n].ssid), lp = sizeof(out[n].pass);
        out[n].pass[0] = '\0';
        if (nvs_get_str(h, ks, out[n].ssid, &ls) != ESP_OK) continue;
        nvs_get_str(h, kp, out[n].pass, &lp);   // pass may be absent (open net)
        n++;
    }
    nvs_close(h);
    return n;
}

esp_err_t net_creds_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t count = 0;
    nvs_get_u8(h, KEY_COUNT, &count);
    if (count >= NET_PROV_MAX_CREDS) count = NET_PROV_MAX_CREDS - 1;  // overwrite last
    char ks[16], kp[16];
    snprintf(ks, sizeof(ks), "ssid%d", count);
    snprintf(kp, sizeof(kp), "pass%d", count);
    err = nvs_set_str(h, ks, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, kp, pass ? pass : "");
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_COUNT, count + 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved cred #%d ssid=%s (%s)", count, ssid, esp_err_to_name(err));
    return err;
}

esp_err_t net_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- event handlers ------------------------------------------------------

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_online = false;
        const wifi_event_sta_disconnected_t *d = data;
        if (s_connecting) {
            ESP_LOGW(TAG, "connect failed (reason=%d)", d ? d->reason : -1);
            xEventGroupSetBits(s_eg, BIT_FAIL);
        } else if (s_auto) {
            ESP_LOGW(TAG, "link dropped (reason=%d); reconnect in %u ms",
                     d ? d->reason : -1, (unsigned)s_backoff_ms);
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_ip = e->ip_info.ip;
        ESP_LOGI(TAG, "online: " IPSTR, IP2STR(&e->ip_info.ip));
        s_online = true;
        s_backoff_ms = BACKOFF_MIN_MS;
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    }
}

static void reconnect_cb(void *arg)
{
    if (s_auto && !s_online) esp_wifi_connect();
}

// ---- public API ----------------------------------------------------------

esp_err_t net_prov_init(void)
{
    if (s_eg) return ESP_OK;  // idempotent
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Idle in power-saving modem-sleep; net_low_latency(true) flips this to WIFI_PS_NONE for the
    // duration of a turn. The MIN_MODEM->NONE flip at turn-start re-initializes the link (re-arms the
    // block-ack session) — keeping it permanently NONE degraded the upload (DELBA storms → upload stall).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    const esp_timer_create_args_t targs = { .callback = reconnect_cb, .name = "net_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));
    ESP_LOGI(TAG, "init done (STA)");
    return ESP_OK;
}

esp_err_t net_connect_saved(uint32_t per_net_timeout_ms)
{
    net_cred_t creds[NET_PROV_MAX_CREDS];
    int n = net_creds_load(creds, NET_PROV_MAX_CREDS);
    if (n == 0) {
        ESP_LOGW(TAG, "no saved credentials");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    for (int i = 0; i < n; i++) {
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, creds[i].ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, creds[i].pass, sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

        ESP_LOGI(TAG, "trying [%d/%d] ssid=%s", i + 1, n, creds[i].ssid);
        xEventGroupClearBits(s_eg, BIT_CONNECTED | BIT_FAIL);
        s_connecting = true;
        esp_wifi_connect();
        EventBits_t bits = xEventGroupWaitBits(
            s_eg, BIT_CONNECTED | BIT_FAIL, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(per_net_timeout_ms));
        s_connecting = false;
        if (bits & BIT_CONNECTED) {
            ESP_LOGI(TAG, "connected to %s", creds[i].ssid);
            return ESP_OK;
        }
        esp_wifi_disconnect();   // early-abort → next network
    }
    ESP_LOGW(TAG, "all %d networks failed", n);
    return ESP_FAIL;
}

bool net_is_connected(void) { return s_online; }

bool net_get_ip_str(char *buf, size_t len)
{
    if (!buf || len < 8 || !s_online) return false;
    snprintf(buf, len, IPSTR, IP2STR(&s_ip));
    return true;
}

void net_start_auto_reconnect(void)
{
    s_auto = true;
    if (!s_online) esp_wifi_connect();
}

void net_low_latency(bool on)
{
    // NONE for the whole turn (mic upload + agent reply) — and the flip itself re-arms the link;
    // MIN_MODEM between turns. Idempotent; applies at runtime once WiFi is started.
    esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

// Idle low-power: power the WiFi radio fully OFF when idle (display off, on battery), and bring it back
// on wake. Saves the modem-sleep idle draw entirely; the next turn waits for net_is_connected() first.
void net_wifi_suspend(void)
{
    s_auto = false;                       // stop the disconnect handler from auto-reconnecting
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();                      // radio off (~0 mA)
    s_online = false;
    ESP_LOGI(TAG, "wifi suspended (radio off)");
}

void net_wifi_resume(void)
{
    s_backoff_ms = BACKOFF_MIN_MS;
    if (esp_wifi_start() == ESP_OK) esp_wifi_set_ps(WIFI_PS_NONE);   // radio on; keep low-latency PS
    s_auto = true;
    esp_wifi_connect();                   // re-associate (async; net_is_connected() flips true on GOT_IP)
    ESP_LOGI(TAG, "wifi resuming (reconnecting)");
}

static volatile bool s_time_synced;   // true once the clock holds real time (SNTP or HTTP fallback)

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    time_t now = time(NULL);
    struct tm utc; gmtime_r(&now, &utc);
    char iso[32]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &utc);
    ESP_LOGI(TAG, "time synced via SNTP: %s", iso);
}

void net_sntp_start(void)
{
    // One-shot SNTP for wall-clock time (P5 ambient context "local_time"). Call once after WiFi is
    // up; it syncs in the background. Guarded so a later reconnect doesn't re-init the service.
    static bool started;
    if (started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sntp_sync;   // log + flag when the clock is actually set
    esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e == ESP_OK) { started = true; ESP_LOGI(TAG, "SNTP started (pool.ntp.org)"); }
    else ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(e));
}

bool net_time_synced(void) { return s_time_synced; }

esp_err_t net_time_http_fallback(void)
{
    // Time-via-HTTPS for networks that block NTP (UDP/123) — common on hotspots/guest WiFi. HEAD a
    // tiny TLS endpoint, parse the server's Date: header (RFC 1123, UTC) -> settimeofday. Cheap; the
    // heartbeat retries it until the clock is set, then stops.
    if (s_time_synced) return ESP_OK;
    esp_http_client_config_t hcfg = {
        .url               = "https://www.google.com/generate_204",
        .method            = HTTP_METHOD_HEAD,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hcfg);
    if (!c) return ESP_FAIL;
    esp_err_t ret = ESP_FAIL;
    if (esp_http_client_perform(c) == ESP_OK) {
        char *date = NULL;
        if (esp_http_client_get_header(c, "Date", &date) == ESP_OK && date) {
            struct tm tm = {0};   // e.g. "Wed, 23 Jun 2026 18:00:00 GMT"
            if (strptime(date, "%a, %d %b %Y %H:%M:%S", &tm)) {
                time_t t = tm_to_utc(&tm);
                if (t > 1700000000) {
                    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                    settimeofday(&tv, NULL);
                    s_time_synced = true;
                    ESP_LOGI(TAG, "time set via HTTPS Date: %s", date);
                    ret = ESP_OK;
                }
            }
        }
    }
    esp_http_client_cleanup(c);
    return ret;
}
