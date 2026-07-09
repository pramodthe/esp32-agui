// soniox_client — see include/soniox_client.h and docs/soniox-rt-protocol.md.

#include "soniox_client.h"
#include "app_cfg.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "soniox";

#define DEFAULT_ENDPOINT "wss://stt-rt.soniox.com/transcribe-websocket"
#define DEFAULT_MODEL    "stt-rt-v5"
#define DEFAULT_SR       16000
#define MIC_GAIN_DB      30.0f
#define READ_CHUNK_BYTES 640           // 320 samples = 20 ms — tight DMA drain (capture frame)
#define DRAIN_CHUNKS     8             // discard ~160ms at capture start (> the 8x300=150ms RX ring)
#define WS_BUFFER_BYTES  8192          // WS tx/rx buffer; send_bin fragments payloads bigger than this
#define SEND_MAX_BYTES   WS_BUFFER_BYTES // send up to one WS_BUFFER in a single transport write...
#define SEND_TRIGGER     4096          // ...but wait for ~128ms of audio first, so each send is large
                                       // enough to amortize TCP delayed-ACK (~200ms/write at this RTT)
#define AUDIO_SB_BYTES   (32 * 1024)   // ~1 s of 16k mono s16le — bounds worst-case latency (PSRAM)
#define COMMITTED_MAX    512
#define RUNNING_MAX      640
#define MSG_MAX          32768         // reject pathologically large server messages

static esp_codec_dev_handle_t        s_mic;
static bool                          s_mic_open;   // s_mic codec dev currently opened (idle stop/start)
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t             s_lock;       // serializes start/finalize/stop
static SemaphoreHandle_t             s_cap_done;   // capture task signals exit
static SemaphoreHandle_t             s_send_done;  // sender task signals exit
static TaskHandle_t                  s_cap_task;
static TaskHandle_t                  s_send_task;
static StreamBufferHandle_t          s_audio_sb;   // mic (producer) -> WSS (consumer), PSRAM
static StaticStreamBuffer_t          s_sb_ctrl;
static uint8_t                      *s_sb_storage; // PSRAM backing for s_audio_sb
static volatile bool                 s_stop;
static volatile bool                 s_fatal;      // fatal Soniox error -> session dead
static volatile bool                 s_need_config;// (re)send config after (re)connect
static volatile bool                 s_active;

static soniox_partial_cb s_partial_cb;
static soniox_turn_cb    s_turn_cb;
static void             *s_ctx;

static char s_api_key[APP_CFG_VAL_MAX];
static char s_model[32];
static int  s_sr;
static char s_committed[COMMITTED_MAX];   // touched only from the ws task
static char s_last_error[128];

// reassembly of multi-event messages (PSRAM)
static uint8_t *s_rx;
static size_t   s_rx_total;

// ---- transcript parsing (runs in ws task) --------------------------------

static void parse_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *errm = cJSON_GetObjectItem(root, "error_message");
    if (cJSON_IsString(errm)) {
        strlcpy(s_last_error, errm->valuestring, sizeof(s_last_error));
        s_fatal = true;                  // stop streaming; surface to the app
        ESP_LOGE(TAG, "soniox error: %s", errm->valuestring);
        cJSON_Delete(root);
        return;
    }

    char tail[RUNNING_MAX];
    tail[0] = '\0';

    cJSON *toks = cJSON_GetObjectItem(root, "tokens");
    if (cJSON_IsArray(toks)) {
        cJSON *t;
        cJSON_ArrayForEach(t, toks) {
            cJSON *txt = cJSON_GetObjectItem(t, "text");
            if (!cJSON_IsString(txt) || !txt->valuestring) continue;
            const char *s = txt->valuestring;
            bool is_final = cJSON_IsTrue(cJSON_GetObjectItem(t, "is_final"));
            if (is_final) {
                if (strcmp(s, "<end>") == 0) {       // utterance boundary
                    if (s_turn_cb) s_turn_cb(s_committed, s_ctx);
                    s_committed[0] = '\0';
                } else {
                    strlcat(s_committed, s, sizeof(s_committed));
                }
            } else {
                strlcat(tail, s, sizeof(tail));
            }
        }
    }

    if (s_partial_cb) {
        char running[RUNNING_MAX];
        snprintf(running, sizeof(running), "%s%s", s_committed, tail);
        if (running[0]) s_partial_cb(running, s_ctx);   // skip empty keepalive messages
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "finished")))
        ESP_LOGI(TAG, "server finished session");

    cJSON_Delete(root);
}

// ---- websocket events ----------------------------------------------------

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        s_committed[0] = '\0';      // fresh transcript context (don't splice across drops)
        s_need_config = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!e || e->payload_len == 0) break;
        if (e->op_code == 0x08 || e->op_code == 0x09 || e->op_code == 0x0A) break; // close/ping/pong
        if (e->payload_len > MSG_MAX) { ESP_LOGW(TAG, "oversized msg %d, dropped", (int)e->payload_len); break; }
        if (e->payload_offset == 0) {                 // first chunk of a message
            if (s_rx) heap_caps_free(s_rx);
            s_rx = heap_caps_malloc(e->payload_len + 1, MALLOC_CAP_SPIRAM);
            s_rx_total = e->payload_len;
        }
        if (!s_rx) break;
        if (e->payload_offset + e->data_len <= s_rx_total)
            memcpy(s_rx + e->payload_offset, e->data_ptr, e->data_len);
        if (e->payload_offset + e->data_len >= s_rx_total) {   // message complete
            s_rx[s_rx_total] = '\0';
            parse_message((const char *)s_rx);
            heap_caps_free(s_rx);
            s_rx = NULL;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws transport error");
        break;
    default: break;
    }
}

// ---- config frame --------------------------------------------------------

static esp_err_t send_config_frame(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "api_key", s_api_key);
    cJSON_AddStringToObject(root, "model", s_model);
    cJSON_AddStringToObject(root, "audio_format", "pcm_s16le");
    cJSON_AddNumberToObject(root, "sample_rate", s_sr);
    cJSON_AddNumberToObject(root, "num_channels", 1);
    cJSON *lh = cJSON_AddArrayToObject(root, "language_hints");
    cJSON_AddItemToArray(lh, cJSON_CreateString("en"));
    cJSON_AddBoolToObject(root, "enable_endpoint_detection", true);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_ERR_NO_MEM;
    int n = esp_websocket_client_send_text(s_ws, s, strlen(s), pdMS_TO_TICKS(2000));
    cJSON_free(s);
    ESP_LOGI(TAG, "sent config frame (%d)", n);
    return n < 0 ? ESP_FAIL : ESP_OK;
}

// ---- capture (producer): mic -> ring buffer ------------------------------
// Tight loop that does NOTHING but drain the I2S DMA into s_audio_sb in small
// 20ms reads. Decoupled from the network so a stalled TLS send can never starve
// the mic (a single read+send task dropped frames to DMA overrun during sends).

static void capture_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(READ_CHUNK_BYTES, MALLOC_CAP_DEFAULT);
    if (buf) {
        // The mic ADC + I2S RX DMA run continuously from boot, so at session start the ring
        // (8x300 ≈ 150ms; auto_clear only clears TX, never RX) holds stale audio — including any
        // PTT-beep crosstalk the shared ES8311 recorded while the beep played. Discard the whole
        // ring so the transcript onset is clean. Also re-assert the mic gain: opening the speaker
        // for the beep soft-resets the shared ES8311's ADC gain register back to its default.
        esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
        for (int i = 0; i < DRAIN_CHUNKS && !s_stop && !s_fatal; i++)
            esp_codec_dev_read(s_mic, buf, READ_CHUNK_BYTES);   // flush stale/crosstalk; content discarded
        int diag_n = 0, diag_peak = 0;                          // DIAG: mic level (silence vs audio)
        while (!s_stop && !s_fatal) {
            if (esp_codec_dev_read(s_mic, buf, READ_CHUNK_BYTES) != ESP_OK) continue;
            const int16_t *sm = (const int16_t *)buf;           // DIAG: track peak |sample| ~1s
            for (int i = 0; i < READ_CHUNK_BYTES / 2; i++) { int a = sm[i]; if (a < 0) a = -a; if (a > diag_peak) diag_peak = a; }
            if (++diag_n >= 50) { ESP_LOGI(TAG, "mic peak=%d", diag_peak); diag_n = 0; diag_peak = 0; }
            // Never block the mic. Drop the *whole* chunk if it won't fit (sender behind):
            // a partial write would split a 16-bit sample and desync 2-byte alignment for
            // the rest of the stream. Dropping a full even-sized chunk keeps alignment.
            if (xStreamBufferSpacesAvailable(s_audio_sb) >= READ_CHUNK_BYTES)
                xStreamBufferSend(s_audio_sb, buf, READ_CHUNK_BYTES, 0);
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGE(TAG, "no mem for capture buf");
    }
    s_cap_task = NULL;
    xSemaphoreGive(s_cap_done);
    vTaskDelete(NULL);
}

// ---- sender (consumer): ring buffer -> binary WS frames -------------------
// Owns s_ws while the session runs. On stop it drains whatever audio remains in
// the ring buffer (the producer has already joined) before signalling done, so
// stop() can safely destroy s_ws.

static void sender_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(SEND_MAX_BYTES, MALLOC_CAP_DEFAULT);
    if (buf) {
        while (true) {
            if (!esp_websocket_client_is_connected(s_ws)) {
                if (s_stop || s_fatal) break;
                vTaskDelay(pdMS_TO_TICKS(50));    // wait for connect; producer keeps buffering
                continue;
            }
            if (s_need_config) {
                if (send_config_frame() == ESP_OK) s_need_config = false;
                else vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            size_t n = xStreamBufferReceive(s_audio_sb, buf, SEND_MAX_BYTES, pdMS_TO_TICKS(100));
            if (n == 0) { if (s_stop || s_fatal) break; continue; }   // stop + drained -> exit
            if (s_fatal) continue;
            // Ignore the return (known-good d0dd24b9 behavior): a slow/failed send just drops this chunk
            // (a small gap) and we move on — don't risk a fatal flag during a reconnect flap.
            esp_websocket_client_send_bin(s_ws, (const char *)buf, n, pdMS_TO_TICKS(500));
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGE(TAG, "no mem for sender buf");
    }
    // From here on we no longer touch s_ws — safe for stop() to destroy it.
    s_send_task = NULL;
    xSemaphoreGive(s_send_done);
    vTaskDelete(NULL);
}

// ---- public API ----------------------------------------------------------

esp_err_t soniox_client_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_cap_done) s_cap_done = xSemaphoreCreateBinary();
    if (!s_send_done) s_send_done = xSemaphoreCreateBinary();
    if (!s_audio_sb) {
        s_sb_storage = heap_caps_malloc(AUDIO_SB_BYTES + 1, MALLOC_CAP_SPIRAM);
        if (!s_sb_storage) { ESP_LOGE(TAG, "no PSRAM for audio ring buffer"); return ESP_ERR_NO_MEM; }
        s_audio_sb = xStreamBufferCreateStatic(AUDIO_SB_BYTES, SEND_TRIGGER, s_sb_storage, &s_sb_ctrl);
    }
    if (s_mic) return ESP_OK;
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) { ESP_LOGE(TAG, "mic init failed"); return ESP_FAIL; }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = DEFAULT_SR,
    };
    esp_err_t err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic open failed: %s", esp_err_to_name(err)); return err; }
    s_mic_open = true;
    ESP_LOGI(TAG, "mic ready (16k mono)");
    return ESP_OK;
}

// Idle low-power: stop the mic codec when idle so the I2S RX channel is disabled and releases its
// NO_LIGHT_SLEEP lock (a running I2S blocks light sleep). esp_codec_dev_close() disables the channel;
// mic_start() reopens it (re-asserting the 30 dB gain) before a turn. Both idempotent.
esp_err_t soniox_client_mic_stop(void)
{
    if (s_mic && s_mic_open) { esp_codec_dev_close(s_mic); s_mic_open = false; }
    return ESP_OK;
}

esp_err_t soniox_client_mic_start(void)
{
    if (!s_mic) return soniox_client_init();        // first-time init opens it
    if (s_mic_open) return ESP_OK;
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = DEFAULT_SR };
    esp_err_t err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic reopen failed: %s", esp_err_to_name(err)); return err; }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    s_mic_open = true;
    return ESP_OK;
}

esp_err_t soniox_session_start(const soniox_cfg_t *cfg,
                               soniox_partial_cb on_partial,
                               soniox_turn_cb on_turn, void *ctx)
{
    if (!s_mic) { esp_err_t e = soniox_client_init(); if (e != ESP_OK) return e; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }

    const char *endpoint = (cfg && cfg->endpoint) ? cfg->endpoint : DEFAULT_ENDPOINT;
    strlcpy(s_model, (cfg && cfg->model) ? cfg->model : DEFAULT_MODEL, sizeof(s_model));
    s_sr = (cfg && cfg->sample_rate) ? cfg->sample_rate : DEFAULT_SR;
    if (cfg && cfg->api_key) {
        strlcpy(s_api_key, cfg->api_key, sizeof(s_api_key));
    } else if (!app_cfg_get(APP_CFG_SONIOX_KEY, s_api_key, sizeof(s_api_key))) {
        ESP_LOGE(TAG, "no usable Soniox API key (missing or too long) — re-enter via the portal");
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    s_partial_cb = on_partial;
    s_turn_cb = on_turn;
    s_ctx = ctx;
    s_committed[0] = '\0';
    s_last_error[0] = '\0';
    s_stop = false;
    s_fatal = false;
    s_need_config = true;
    xSemaphoreTake(s_cap_done, 0);    // drain any stale signal from a prior session
    xSemaphoreTake(s_send_done, 0);
    xStreamBufferReset(s_audio_sb);   // discard any audio left from a prior session

    esp_websocket_client_config_t wcfg = {
        .uri = endpoint,
        .buffer_size = WS_BUFFER_BYTES,   // big enough that one batched audio send = one transport write
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
    };
    s_ws = esp_websocket_client_init(&wcfg);
    if (!s_ws) { xSemaphoreGive(s_lock); return ESP_FAIL; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) { esp_websocket_client_destroy(s_ws); s_ws = NULL; xSemaphoreGive(s_lock); return err; }

    s_active = true;
    // Producer at higher priority than consumer: draining the mic DMA is real-time
    // critical; the network send can wait.
    if (xTaskCreate(capture_task, "soniox_cap", 4096, NULL, 6, &s_cap_task) != pdPASS ||
        xTaskCreate(sender_task,  "soniox_snd", 4096, NULL, 5, &s_send_task) != pdPASS) {
        s_stop = true;
        if (s_cap_task) xSemaphoreTake(s_cap_done, portMAX_DELAY);
        if (s_send_task) xSemaphoreTake(s_send_done, portMAX_DELAY);
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_active = false;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "session started (%s, %s, %d Hz)", endpoint, s_model, s_sr);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t soniox_session_finalize(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_active && s_ws) {
        const char *msg = "{\"type\":\"finalize\"}";
        int n = esp_websocket_client_send_text(s_ws, msg, strlen(msg), pdMS_TO_TICKS(1000));
        ret = n < 0 ? ESP_FAIL : ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void soniox_session_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_active) { xSemaphoreGive(s_lock); return; }
    s_stop = true;
    // Join producer first (stops filling the buffer), then the sender, which drains
    // whatever audio remains before it stops touching s_ws.
    if (s_cap_task) xSemaphoreTake(s_cap_done, portMAX_DELAY);
    if (s_send_task) xSemaphoreTake(s_send_done, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;   // tasks no longer touch s_ws
    s_ws = NULL;
    if (ws) {
        // Close the socket directly (destroy() → TCP FIN). Deliberately do NOT send Soniox's
        // empty end-of-audio frame here: that makes Soniox initiate a WS CLOSE, which the client
        // echoes and then waits on a hardcoded 1 s TCP-close poll that logs "Did not get TCP close
        // within expected delay" and stalls the stop. The turn is already committed (Soniox
        // '<end>'), so a direct TCP close cleanly ends the session with no warning and no stall.
        esp_websocket_client_destroy(ws);
    }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
    s_active = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "session stopped");
}

bool soniox_session_active(void) { return s_active && !s_fatal; }

const char *soniox_last_error(void) { return s_last_error[0] ? s_last_error : NULL; }
