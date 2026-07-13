// deepgram_stt — mic (16 kHz mono s16le) -> Deepgram Listen v1 WSS -> transcript callbacks.
#include "deepgram_stt.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"   // xTaskCreateWithCaps — STT stacks in PSRAM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "dg_stt";

#define DEFAULT_MODEL    "nova-3"
#define DEFAULT_SR       16000
#define MIC_GAIN_DB      30.0f
#define READ_CHUNK_BYTES 640
#define DRAIN_CHUNKS     8
// Keep WS buffers/stack modest: rx+tx are calloc'd in *internal* RAM, and the ws
// task stack is too. After AG-UI TLS, an 8 KB stack often fails → "websocket start: ESP_FAIL".
#define WS_BUFFER_BYTES  4096
#define WS_TASK_STACK    5120
#define SEND_MAX_BYTES   WS_BUFFER_BYTES
#define SEND_TRIGGER     2048
#define AUDIO_SB_BYTES   (32 * 1024)
#define COMMITTED_MAX    512
#define RUNNING_MAX      640
#define MSG_MAX          32768
#define URI_MAX          320
#define AUTH_HDR_MAX     (APP_CFG_VAL_MAX + 32)

// APP_CFG_VAL_MAX lives in app_cfg.h — pull for auth buffer sizing
#include "app_cfg.h"

static esp_codec_dev_handle_t        s_mic;
static bool                          s_mic_open;
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t             s_lock;
static SemaphoreHandle_t             s_cap_done;
static SemaphoreHandle_t             s_send_done;
static TaskHandle_t                  s_cap_task;
static TaskHandle_t                  s_send_task;
static StreamBufferHandle_t          s_audio_sb;
static StaticStreamBuffer_t          s_sb_ctrl;
static uint8_t                      *s_sb_storage;
static volatile bool                 s_stop;
static volatile bool                 s_fatal;
static volatile bool                 s_active;
static volatile bool                 s_close_sent;

static deepgram_stt_partial_cb s_partial_cb;
static deepgram_stt_turn_cb    s_turn_cb;
static void                   *s_ctx;

static char s_api_key[APP_CFG_VAL_MAX];
static char s_model[48];
static int  s_sr;
static char s_uri[URI_MAX];
static char s_auth_hdr[AUTH_HDR_MAX];
static char s_committed[COMMITTED_MAX];  // touched only from the ws task (same invariant as Soniox)
static char s_last_error[128];

static uint8_t *s_rx;
static size_t   s_rx_total;
static size_t   s_rx_written;   // bytes copied into s_rx so far; parse only when it reaches s_rx_total

static void emit_partial(const char *interim)
{
    if (!s_partial_cb) return;
    char running[RUNNING_MAX];
    snprintf(running, sizeof running, "%s%s", s_committed, interim ? interim : "");
    if (running[0]) s_partial_cb(running, s_ctx);
}

static void commit_turn(void)
{
    if (s_committed[0] && s_turn_cb) s_turn_cb(s_committed, s_ctx);
    s_committed[0] = '\0';
}

static void parse_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(err) && err->valuestring) {
        strlcpy(s_last_error, err->valuestring, sizeof s_last_error);
        s_fatal = true;
        ESP_LOGE(TAG, "deepgram error: %s", err->valuestring);
        cJSON_Delete(root);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "Results") == 0) {
        cJSON *ch = cJSON_GetObjectItem(root, "channel");
        cJSON *alts = ch ? cJSON_GetObjectItem(ch, "alternatives") : NULL;
        cJSON *alt0 = (cJSON_IsArray(alts) && cJSON_GetArraySize(alts) > 0)
                          ? cJSON_GetArrayItem(alts, 0) : NULL;
        cJSON *tr = alt0 ? cJSON_GetObjectItem(alt0, "transcript") : NULL;
        const char *transcript = (cJSON_IsString(tr) && tr->valuestring) ? tr->valuestring : "";
        bool is_final = cJSON_IsTrue(cJSON_GetObjectItem(root, "is_final"));
        bool speech_final = cJSON_IsTrue(cJSON_GetObjectItem(root, "speech_final"));

        if (is_final) {
            if (transcript[0]) {
                if (s_committed[0] && s_committed[strlen(s_committed) - 1] != ' ')
                    strlcat(s_committed, " ", sizeof s_committed);
                strlcat(s_committed, transcript, sizeof s_committed);
            }
            emit_partial("");
            if (speech_final) commit_turn();
        } else {
            emit_partial(transcript);
        }
    } else if (strcmp(t, "UtteranceEnd") == 0) {
        commit_turn();
        emit_partial("");
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
        s_committed[0] = '\0';
        s_close_sent = false;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!e || e->payload_len == 0) break;
        if (e->op_code == 0x08 || e->op_code == 0x09 || e->op_code == 0x0A) break;
        if (e->op_code == 0x02) break; // ignore unexpected binary from server
        if (e->payload_len > MSG_MAX) { ESP_LOGW(TAG, "oversized msg %d", (int)e->payload_len); break; }
        if (e->payload_offset == 0) {
            if (s_rx) heap_caps_free(s_rx);
            s_rx = heap_caps_malloc(e->payload_len + 1, MALLOC_CAP_SPIRAM);
            s_rx_total = e->payload_len;
            s_rx_written = 0;
        }
        if (!s_rx) break;
        if (e->payload_offset + e->data_len <= s_rx_total) {
            memcpy(s_rx + e->payload_offset, e->data_ptr, e->data_len);
            s_rx_written += e->data_len;
        }
        // Parse only when the whole message is assembled. Gate on bytes actually written (not on the
        // offset reaching the end): an overshooting fragment is skipped above, so keying off the offset
        // would parse a buffer with an unfilled hole → garbled/dropped transcript.
        if (s_rx_written >= s_rx_total) {
            s_rx[s_rx_total] = '\0';
            parse_message((const char *)s_rx);
            heap_caps_free(s_rx);
            s_rx = NULL;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws transport error");
        if (!s_last_error[0])
            strlcpy(s_last_error, "Deepgram websocket failed", sizeof s_last_error);
        s_fatal = true;
        break;
    default: break;
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(READ_CHUNK_BYTES, MALLOC_CAP_DEFAULT);
    if (buf) {
        esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
        for (int i = 0; i < DRAIN_CHUNKS && !s_stop && !s_fatal; i++)
            esp_codec_dev_read(s_mic, buf, READ_CHUNK_BYTES);
        while (!s_stop && !s_fatal) {
            if (esp_codec_dev_read(s_mic, buf, READ_CHUNK_BYTES) != ESP_OK) continue;
            if (xStreamBufferSpacesAvailable(s_audio_sb) >= READ_CHUNK_BYTES)
                xStreamBufferSend(s_audio_sb, buf, READ_CHUNK_BYTES, 0);
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGE(TAG, "no mem for capture buf");
    }
    s_cap_task = NULL;
    xSemaphoreGive(s_cap_done);
    vTaskDeleteWithCaps(NULL);
}

static void sender_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(SEND_MAX_BYTES, MALLOC_CAP_DEFAULT);
    if (buf) {
        while (true) {
            if (!esp_websocket_client_is_connected(s_ws)) {
                if (s_stop || s_fatal) break;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            size_t n = xStreamBufferReceive(s_audio_sb, buf, SEND_MAX_BYTES, pdMS_TO_TICKS(100));
            if (n == 0) {
                if (s_stop || s_fatal) break;
                continue;
            }
            if (s_fatal) continue;
            esp_websocket_client_send_bin(s_ws, (const char *)buf, n, pdMS_TO_TICKS(500));
        }
        // End-of-audio: ask Deepgram to flush finals before we tear the socket down.
        if (s_ws && esp_websocket_client_is_connected(s_ws) && !s_close_sent) {
            const char *msg = "{\"type\":\"CloseStream\"}";
            esp_websocket_client_send_text(s_ws, msg, strlen(msg), pdMS_TO_TICKS(1000));
            s_close_sent = true;
            // Brief wait for final Results / UtteranceEnd on the ws task.
            // Do NOT commit_turn() here — s_committed is ws-task-only (same as Soniox).
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGE(TAG, "no mem for sender buf");
    }
    s_send_task = NULL;
    xSemaphoreGive(s_send_done);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t deepgram_stt_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_cap_done) s_cap_done = xSemaphoreCreateBinary();
    if (!s_send_done) s_send_done = xSemaphoreCreateBinary();
    if (!s_audio_sb) {
        s_sb_storage = heap_caps_malloc(AUDIO_SB_BYTES + 1, MALLOC_CAP_SPIRAM);
        if (!s_sb_storage) { ESP_LOGE(TAG, "no PSRAM for audio ring"); return ESP_ERR_NO_MEM; }
        s_audio_sb = xStreamBufferCreateStatic(AUDIO_SB_BYTES, SEND_TRIGGER, s_sb_storage, &s_sb_ctrl);
    }
    if (s_mic) return ESP_OK;
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) { ESP_LOGE(TAG, "mic init failed"); return ESP_FAIL; }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 1, .sample_rate = DEFAULT_SR,
    };
    esp_err_t err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic open failed: %s", esp_err_to_name(err)); return err; }
    s_mic_open = true;
    ESP_LOGI(TAG, "mic ready (16k mono)");
    return ESP_OK;
}

void deepgram_stt_deinit(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "deinit while session active — stop first");
        deepgram_stt_session_stop();
    }
    if (s_mic) {
        if (s_mic_open) { esp_codec_dev_close(s_mic); s_mic_open = false; }
        esp_codec_dev_delete(s_mic);
        s_mic = NULL;
        ESP_LOGI(TAG, "mic released");
    }
}

esp_err_t deepgram_stt_mic_stop(void)
{
    if (s_mic && s_mic_open) { esp_codec_dev_close(s_mic); s_mic_open = false; }
    return ESP_OK;
}

esp_err_t deepgram_stt_mic_start(void)
{
    if (!s_mic) return deepgram_stt_init();
    if (s_mic_open) return ESP_OK;
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = DEFAULT_SR };
    esp_err_t err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic reopen failed: %s", esp_err_to_name(err)); return err; }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    s_mic_open = true;
    return ESP_OK;
}

esp_err_t deepgram_stt_session_start(const deepgram_stt_cfg_t *cfg,
                                     deepgram_stt_partial_cb on_partial,
                                     deepgram_stt_turn_cb on_turn, void *ctx)
{
    if (!s_mic) { esp_err_t e = deepgram_stt_init(); if (e != ESP_OK) return e; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        strlcpy(s_last_error, "STT session already active", sizeof s_last_error);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (!cfg || !cfg->api_key || !cfg->api_key[0]) {
        ESP_LOGE(TAG, "missing Deepgram API key");
        strlcpy(s_last_error, "no Deepgram API key", sizeof s_last_error);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(s_api_key, cfg->api_key, sizeof s_api_key);
    strlcpy(s_model, (cfg->model && cfg->model[0]) ? cfg->model : DEFAULT_MODEL, sizeof s_model);
    s_sr = cfg->sample_rate ? cfg->sample_rate : DEFAULT_SR;

    if (cfg->endpoint && cfg->endpoint[0]) {
        strlcpy(s_uri, cfg->endpoint, sizeof s_uri);
    } else {
        snprintf(s_uri, sizeof s_uri,
                 "wss://api.deepgram.com/v1/listen?model=%s&encoding=linear16"
                 "&sample_rate=%d&channels=1&interim_results=true&punctuate=true"
                 "&endpointing=300&utterance_end_ms=1000",
                 s_model, s_sr);
    }
    snprintf(s_auth_hdr, sizeof s_auth_hdr, "Authorization: Token %s\r\n", s_api_key);

    s_partial_cb = on_partial;
    s_turn_cb = on_turn;
    s_ctx = ctx;
    s_committed[0] = '\0';
    s_last_error[0] = '\0';
    s_stop = false;
    s_fatal = false;
    s_close_sent = false;
    xSemaphoreTake(s_cap_done, 0);
    xSemaphoreTake(s_send_done, 0);
    xStreamBufferReset(s_audio_sb);

    esp_websocket_client_config_t wcfg = {
        .uri = s_uri,
        .headers = s_auth_hdr,
        .buffer_size = WS_BUFFER_BYTES,
        .task_stack = WS_TASK_STACK,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,   // we own session lifecycle; reconnect fights stop/destroy
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
    };
    s_ws = esp_websocket_client_init(&wcfg);
    if (!s_ws) {
        strlcpy(s_last_error, "websocket init failed", sizeof s_last_error);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        // One settle+retry: prior AG-UI/TTS TLS often leaves internal heap fragmented for a beat.
        ESP_LOGW(TAG, "ws start %s (int free=%u largest=%u) — retry",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(150));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_active) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
        s_ws = esp_websocket_client_init(&wcfg);
        if (s_ws) {
            esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
            err = esp_websocket_client_start(s_ws);
        } else {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            snprintf(s_last_error, sizeof s_last_error, "websocket start: %s", esp_err_to_name(err));
            if (s_ws) { esp_websocket_client_destroy(s_ws); s_ws = NULL; }
            xSemaphoreGive(s_lock);
            return err;
        }
    }

    s_active = true;
    // Stacks in PSRAM so websocket (internal) + capture/sender can coexist after AG-UI/TTS TLS.
    if (xTaskCreateWithCaps(capture_task, "dg_cap", 4096, NULL, 6, &s_cap_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreateWithCaps(sender_task,  "dg_snd", 4096, NULL, 5, &s_send_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_stop = true;
        if (s_cap_task) xSemaphoreTake(s_cap_done, portMAX_DELAY);
        if (s_send_task) xSemaphoreTake(s_send_done, portMAX_DELAY);
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_active = false;
        strlcpy(s_last_error, "STT tasks failed", sizeof s_last_error);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    xSemaphoreGive(s_lock);

    // Wait briefly for TLS/WSS so a broken hotspot/key surfaces as start failure, not silent listen.
    for (int i = 0; i < 100; i++) {   // ~5 s
        if (s_fatal) break;
        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            ESP_LOGI(TAG, "session started (%s @ %d Hz)", s_model, s_sr);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_last_error[0])
        strlcpy(s_last_error, "Deepgram connect timeout", sizeof s_last_error);
    ESP_LOGE(TAG, "ws not connected: %s", s_last_error);
    deepgram_stt_session_stop();
    return ESP_FAIL;
}

esp_err_t deepgram_stt_session_finalize(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_active && s_ws && !s_close_sent) {
        const char *msg = "{\"type\":\"Finalize\"}";
        int n = esp_websocket_client_send_text(s_ws, msg, strlen(msg), pdMS_TO_TICKS(1000));
        ret = n < 0 ? ESP_FAIL : ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void deepgram_stt_session_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_active) { xSemaphoreGive(s_lock); return; }
    s_stop = true;
    if (s_cap_task) xSemaphoreTake(s_cap_done, portMAX_DELAY);
    if (s_send_task) xSemaphoreTake(s_send_done, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    if (ws) {
        esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
    }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
    s_active = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "session stopped");
}

// Report the raw session state: a fatal server error sets s_fatal but leaves the ws/tasks/mic up, so
// the session still needs an explicit stop — masking it with !s_fatal would let callers skip teardown
// and leak the ES8311 mic (next session_start then returns ESP_ERR_INVALID_STATE). Error text is
// surfaced separately via deepgram_stt_last_error().
bool deepgram_stt_session_active(void) { return s_active; }

const char *deepgram_stt_last_error(void) { return s_last_error[0] ? s_last_error : NULL; }
