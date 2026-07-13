// deepgram_tts — text -> Deepgram Speak v1 WSS -> pcm_s16le @16k/mono -> sink.
#include "deepgram_tts.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "app_cfg.h"
#include "speech_cfg.h"

static const char *TAG = "dg_tts";

#define TTS_VOICE_DEFAULT "aura-2-asteria-en"
#define TTS_SR            16000
#define WS_BUFFER_BYTES   8192
#define MSG_MAX           65536
#define RING_BYTES        (128 * 1024)
#define DRAIN_CHUNK       640
#define URI_MAX           320
#define AUTH_HDR_MAX      (APP_CFG_VAL_MAX + 32)

#define BIT_CONNECTED  (1u << 0)
#define BIT_FLUSHED    (1u << 1)
#define BIT_WSERR      (1u << 2)
#define BIT_CANCEL     (1u << 3)
#define BIT_CLOSED     (1u << 4)
#define BIT_DRAIN_EXIT (1u << 5)

typedef enum { TTS_IDLE, TTS_OPEN, TTS_FINISHING } tts_state_t;
static tts_state_t                    s_state;
static volatile bool                  s_cancel;
static volatile uint32_t              s_audio_rx;
static SemaphoreHandle_t              s_send_mutex;
static void (*s_sink)(const void *pcm, size_t bytes);
static esp_websocket_client_handle_t  s_ws;
static StreamBufferHandle_t           s_ring;
static StaticStreamBuffer_t           s_ring_ctrl;
static uint8_t                       *s_ring_buf;
static EventGroupHandle_t             s_eg;
static SemaphoreHandle_t              s_lock;
static TaskHandle_t                   s_drain_task;
static uint8_t                       *s_rx;
static size_t                         s_rx_total;
static char                           s_api_key[APP_CFG_VAL_MAX];
static char                           s_voice[APP_CFG_VAL_MAX];
static char                           s_uri[URI_MAX];
static char                           s_auth_hdr[AUTH_HDR_MAX];

static void drain_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(DRAIN_CHUNK, MALLOC_CAP_DEFAULT);
    if (!buf) { ESP_LOGE(TAG, "drain buf alloc failed"); s_drain_task = NULL; vTaskDelete(NULL); return; }
    for (;;) {
        if (s_eg && (xEventGroupGetBits(s_eg) & BIT_DRAIN_EXIT)) break;
        size_t n = xStreamBufferReceive(s_ring, buf, DRAIN_CHUNK, pdMS_TO_TICKS(100));
        if (s_cancel) continue;
        if (n && s_sink) s_sink(buf, n);
    }
    heap_caps_free(buf);
    s_drain_task = NULL;
    vTaskDelete(NULL);
}

static void parse_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(t, "Flushed") == 0) {
        xEventGroupSetBits(s_eg, BIT_FLUSHED);
    } else if (strcmp(t, "Warning") == 0) {
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        ESP_LOGW(TAG, "tts warning: %s", cJSON_IsString(desc) ? desc->valuestring : json);
    } else if (cJSON_GetObjectItem(root, "error") || strcmp(t, "Error") == 0) {
        ESP_LOGW(TAG, "tts server error: %s", json);
        xEventGroupSetBits(s_eg, BIT_WSERR);
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupSetBits(s_eg, BIT_CLOSED);
        if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!e || e->payload_len == 0) break;
        if (e->op_code == 0x08 || e->op_code == 0x09 || e->op_code == 0x0A) break;
        // Binary = PCM audio
        if (e->op_code == 0x02) {
            if (e->data_len && !s_cancel) {
                xStreamBufferSend(s_ring, e->data_ptr, e->data_len, pdMS_TO_TICKS(2000));
                s_audio_rx += e->data_len;
            }
            break;
        }
        // Text = Metadata / Flushed / Warning
        if (e->payload_len > MSG_MAX) break;
        if (e->payload_offset == 0) {
            if (s_rx) heap_caps_free(s_rx);
            s_rx = heap_caps_malloc(e->payload_len + 1, MALLOC_CAP_SPIRAM);
            s_rx_total = e->payload_len;
        }
        if (!s_rx) break;
        if (e->payload_offset + e->data_len <= s_rx_total)
            memcpy(s_rx + e->payload_offset, e->data_ptr, e->data_len);
        if (e->payload_offset + e->data_len >= s_rx_total) {
            s_rx[s_rx_total] = '\0';
            parse_json((const char *)s_rx);
            heap_caps_free(s_rx);
            s_rx = NULL;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws transport error");
        xEventGroupSetBits(s_eg, BIT_WSERR);
        break;
    default: break;
    }
}

esp_err_t deepgram_tts_init(void (*sink)(const void *pcm, size_t bytes))
{
    s_sink = sink;
    if (!s_eg) s_eg = xEventGroupCreate();
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_send_mutex) s_send_mutex = xSemaphoreCreateMutex();
    if (!s_eg || !s_lock || !s_send_mutex) {
        ESP_LOGE(TAG, "init alloc failed");
        return ESP_ERR_NO_MEM;
    }
    if (!s_ring_buf) {
        s_ring_buf = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_ring_buf) { ESP_LOGE(TAG, "ring alloc failed"); return ESP_ERR_NO_MEM; }
        s_ring = xStreamBufferCreateStatic(RING_BYTES, 1, s_ring_buf, &s_ring_ctrl);
        if (!s_ring) return ESP_ERR_NO_MEM;
    }
    if (s_drain_task) return ESP_OK;
    xEventGroupClearBits(s_eg, BIT_DRAIN_EXIT);
    if (xTaskCreate(drain_task, "dg_tts_drain", 3072, NULL, 5, &s_drain_task) != pdPASS) {
        ESP_LOGE(TAG, "drain task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "tts inited (ring %d KB PSRAM)", RING_BYTES / 1024);
    return ESP_OK;
}

void deepgram_tts_deinit(void)
{
    if (!s_eg && !s_drain_task) return;
    deepgram_tts_cancel();
    if (s_eg) xEventGroupSetBits(s_eg, BIT_DRAIN_EXIT);
    for (int i = 0; i < 50 && s_drain_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_ws) { esp_websocket_client_stop(s_ws); esp_websocket_client_destroy(s_ws); s_ws = NULL; }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
    if (s_ring_buf) { heap_caps_free(s_ring_buf); s_ring_buf = NULL; }
    s_ring = NULL;
    s_state = TTS_IDLE;
    s_sink = NULL;
    ESP_LOGI(TAG, "tts deinit");
}

static esp_err_t send_json_locked(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int n = esp_websocket_client_send_text(s_ws, s, strlen(s), pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_send_mutex);
    cJSON_free(s);
    return n < 0 ? ESP_FAIL : ESP_OK;
}

static void ws_teardown(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
}

esp_err_t deepgram_tts_open(void)
{
    if (!s_lock || !s_sink) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!speech_cfg_get_key(s_api_key, sizeof s_api_key)) {
        ESP_LOGW(TAG, "no speech API key for TTS");
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (!app_cfg_get(APP_CFG_TTS_VOICE, s_voice, sizeof s_voice) || !s_voice[0])
        strlcpy(s_voice, TTS_VOICE_DEFAULT, sizeof s_voice);

    s_cancel = false;
    s_audio_rx = 0;
    s_state = TTS_IDLE;
    xStreamBufferReset(s_ring);
    xEventGroupClearBits(s_eg, BIT_CONNECTED | BIT_FLUSHED | BIT_WSERR | BIT_CANCEL | BIT_CLOSED);

    snprintf(s_uri, sizeof s_uri,
             "wss://api.deepgram.com/v1/speak?model=%s&encoding=linear16&sample_rate=%d",
             s_voice, TTS_SR);
    snprintf(s_auth_hdr, sizeof s_auth_hdr, "Authorization: Token %s\r\n", s_api_key);

    ESP_LOGI(TAG, "open: internal free %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    esp_websocket_client_config_t wcfg = {
        .uri = s_uri,
        .headers = s_auth_hdr,
        .buffer_size = WS_BUFFER_BYTES,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
    };
    s_ws = esp_websocket_client_init(&wcfg);
    if (!s_ws) { xSemaphoreGive(s_lock); return ESP_FAIL; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) { ws_teardown(); xSemaphoreGive(s_lock); return ESP_FAIL; }

    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_CONNECTED | BIT_WSERR | BIT_CANCEL,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(b & BIT_CONNECTED)) {
        ESP_LOGW(TAG, "tts connect failed/timeout → batch");
        ws_teardown();
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    s_state = TTS_OPEN;
    ESP_LOGI(TAG, "tts stream open (%s)", s_voice);
    return ESP_OK;
}

esp_err_t deepgram_tts_feed(const char *text)
{
    if (s_state != TTS_OPEN || !text || !text[0]) return ESP_ERR_INVALID_STATE;
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "Speak");
    cJSON_AddStringToObject(t, "text", text);
    return send_json_locked(t);
}

esp_err_t deepgram_tts_finish(void)
{
    if (s_state != TTS_OPEN) return ESP_OK;
    cJSON *flush = cJSON_CreateObject();
    cJSON_AddStringToObject(flush, "type", "Flush");
    esp_err_t r = send_json_locked(flush);
    cJSON *close = cJSON_CreateObject();
    cJSON_AddStringToObject(close, "type", "Close");
    send_json_locked(close);
    s_state = TTS_FINISHING;
    return r;
}

esp_err_t deepgram_tts_wait_drained(uint32_t stall_ms)
{
    if (s_state == TTS_IDLE) return ESP_OK;

    for (;;) {
        uint32_t rx_before = s_audio_rx;
        EventBits_t b = xEventGroupWaitBits(s_eg, BIT_FLUSHED | BIT_CLOSED | BIT_WSERR | BIT_CANCEL,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(stall_ms));
        if (b & BIT_CANCEL) { ESP_LOGI(TAG, "tts cancelled (barge-in)"); break; }
        if (b & BIT_WSERR)  { ESP_LOGW(TAG, "tts error"); break; }
        if ((b & BIT_FLUSHED) || (b & BIT_CLOSED)) {
            int spins = 0;
            while (xStreamBufferBytesAvailable(s_ring) > 0 && spins++ < 600)
                vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(120));
            break;
        }
        if (s_audio_rx != rx_before) continue;
        ESP_LOGW(TAG, "tts drain stall (no audio for %u ms)", (unsigned)stall_ms);
        break;
    }

    ws_teardown();
    if (s_cancel) {
        int spins = 0;
        while (xStreamBufferBytesAvailable(s_ring) > 0 && spins++ < 200)
            vTaskDelay(pdMS_TO_TICKS(2));
        s_cancel = false;
    }
    s_state = TTS_IDLE;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t deepgram_tts_speak(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t e = deepgram_tts_open();
    if (e != ESP_OK) return e;
    deepgram_tts_feed(text);
    deepgram_tts_finish();
    return deepgram_tts_wait_drained(30000);
}

void deepgram_tts_cancel(void)
{
    if (!s_eg) return;
    s_cancel = true;
    xEventGroupSetBits(s_eg, BIT_CANCEL);
}
