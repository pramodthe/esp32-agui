// soniox_tts_client — P-a BATCH spoken replies via Soniox real-time TTS.
//
// Mirrors soniox_client's WSS plumbing (esp_websocket_client + crt bundle + a PSRAM ring + a drain
// task) but in reverse: text goes UP, base64 pcm_s16le comes DOWN. Flow of soniox_tts_speak():
//   open WSS -> config frame -> {text, text_end:true} -> RX {audio} frames (base64 -> PSRAM ring,
//   drain task plays via the sink) -> {terminated:true} -> wait ring empty -> close.
// Opened only AFTER the AG-UI run has finished, so the TTS TLS never overlaps the SSE TLS (P-a
// "sequential TLS"); streaming-while-the-run-speaks is P-b.
#include "soniox_tts_client.h"

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
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "app_cfg.h"

static const char *TAG = "tts";

#define TTS_ENDPOINT  "wss://tts-rt.soniox.com/tts-websocket"
#define TTS_MODEL     "tts-rt-v1"
#define TTS_VOICE     "Adrian"             // default when no voice is configured in the portal (NVS)
#define TTS_SR        16000
#define TTS_STREAM_ID "1"

#define WS_BUFFER_BYTES  8192
#define MSG_MAX          65536              // a single base64 audio frame can be large
#define RING_BYTES       (128 * 1024)       // PSRAM PCM ring (~4 s @16k/16/mono)
#define DRAIN_CHUNK      640                // 20 ms @16k mono -> one esp_codec_dev_write

#define BIT_CONNECTED  (1u << 0)
#define BIT_TERMINATED (1u << 1)
#define BIT_WSERR      (1u << 2)
#define BIT_CANCEL     (1u << 3)            // PTT barge-in: stop speaking NOW (set by soniox_tts_cancel)

typedef enum { TTS_IDLE, TTS_OPEN, TTS_FINISHING } tts_state_t;
static tts_state_t                    s_state;    // lifecycle (ptt_task only); IDLE between turns
static volatile bool                  s_cancel;   // mirror of BIT_CANCEL the drain task polls cheaply
static volatile uint32_t              s_audio_rx; // total audio bytes received this stream (stall detect)
static SemaphoreHandle_t              s_send_mutex; // serialises WS writes (feed/finish + P-b2 keepalive)
static tts_pcm_sink_t                 s_sink;
static esp_websocket_client_handle_t  s_ws;
static StreamBufferHandle_t           s_ring;
static StaticStreamBuffer_t           s_ring_ctrl;
static uint8_t                       *s_ring_buf;
static EventGroupHandle_t             s_eg;
static SemaphoreHandle_t              s_lock;
static char                          *s_rx;       // WS message reassembly (PSRAM)
static size_t                         s_rx_total;
static char                           s_api_key[APP_CFG_VAL_MAX];
static char                           s_voice[APP_CFG_VAL_MAX];   // portal-selected TTS voice (default Adrian)

// ---- drain task: ring -> speaker, at real time -----------------------------

static void drain_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(DRAIN_CHUNK, MALLOC_CAP_DEFAULT);
    if (!buf) { ESP_LOGE(TAG, "drain buf alloc failed"); vTaskDelete(NULL); return; }
    for (;;) {
        size_t n = xStreamBufferReceive(s_ring, buf, DRAIN_CHUNK, pdMS_TO_TICKS(100));
        // Barge-in: keep RECEIVING (so the ring drains and speak() can finish) but DISCARD — don't
        // feed the codec. This empties the ring deterministically without an xStreamBufferReset
        // (which would no-op while we're blocked on the buffer). Audio stops within ~1 chunk.
        if (s_cancel) continue;
        if (n && s_sink) s_sink(buf, n);
    }
}

// ---- websocket RX ----------------------------------------------------------

static void parse_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *audio = cJSON_GetObjectItem(root, "audio");
    if (cJSON_IsString(audio) && audio->valuestring && audio->valuestring[0]) {
        size_t b64 = strlen(audio->valuestring);
        size_t cap = (b64 / 4) * 3 + 4;
        uint8_t *pcm = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (pcm) {
            size_t olen = 0;
            if (mbedtls_base64_decode(pcm, cap, &olen,
                                      (const uint8_t *)audio->valuestring, b64) == 0 && olen) {
                // Back-pressure: if the ring is full (audio faster than playback) this blocks the
                // WS task until the drain task frees space — this is what throttles Soniox to ~playback
                // rate, so {terminated} only arrives at the END of a long reply.
                xStreamBufferSend(s_ring, pcm, olen, pdMS_TO_TICKS(2000));
                s_audio_rx += olen;                  // progress signal for wait_drained's stall detector
            } else {
                ESP_LOGW(TAG, "base64 decode failed");
            }
            heap_caps_free(pcm);
        }
    }

    if (cJSON_GetObjectItem(root, "error_code") || cJSON_GetObjectItem(root, "error_message")) {
        ESP_LOGW(TAG, "tts server error: %s", json);
        xEventGroupSetBits(s_eg, BIT_WSERR);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "terminated")))
        xEventGroupSetBits(s_eg, BIT_TERMINATED);

    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!e || e->payload_len == 0) break;
        if (e->op_code == 0x08 || e->op_code == 0x09 || e->op_code == 0x0A) break; // close/ping/pong
        if (e->payload_len > MSG_MAX) { ESP_LOGW(TAG, "oversized msg %d", (int)e->payload_len); break; }
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
            parse_message((const char *)s_rx);
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

// ---- public API ------------------------------------------------------------

esp_err_t soniox_tts_init(tts_pcm_sink_t sink)
{
    if (s_lock) return ESP_OK;            // already inited
    s_sink = sink;
    s_eg = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    s_send_mutex = xSemaphoreCreateMutex();
    s_ring_buf = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_eg || !s_lock || !s_send_mutex || !s_ring_buf) { ESP_LOGE(TAG, "init alloc failed"); return ESP_ERR_NO_MEM; }
    s_ring = xStreamBufferCreateStatic(RING_BYTES, 1, s_ring_buf, &s_ring_ctrl);
    if (!s_ring) return ESP_ERR_NO_MEM;
    if (xTaskCreate(drain_task, "tts_drain", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "drain task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "tts inited (ring %d KB PSRAM)", RING_BYTES / 1024);
    return ESP_OK;
}

static esp_err_t send_json_locked(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);   // one payload at a time on the wire (feed/finish/keepalive)
    int n = esp_websocket_client_send_text(s_ws, s, strlen(s), pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_send_mutex);
    cJSON_free(s);
    return n < 0 ? ESP_FAIL : ESP_OK;
}

static void ws_teardown(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);            // WS task exits here → no more audio enters the ring
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
}

// ---- streaming API (P-b): open -> feed* -> finish -> wait_drained ----------------
// open() BLOCKS through the WSS handshake + config (~handshake latency); it is called from the AG-UI
// SSE on_text_delta handler on ptt_task, so it briefly stalls the SSE read (data buffers, no loss) but
// keeps the path simple/lossless. On success it holds s_lock until wait_drained() releases it. On ANY
// failure it tears everything down, releases s_lock, and returns an error so the caller falls back to
// batch — so a concurrent-TLS OOM degrades gracefully, it doesn't crash.

esp_err_t soniox_tts_open(void)
{
    if (!s_lock || !s_sink) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!app_cfg_get(APP_CFG_SONIOX_KEY, s_api_key, sizeof s_api_key)) {
        ESP_LOGW(TAG, "no Soniox key for TTS");
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (!app_cfg_get(APP_CFG_TTS_VOICE, s_voice, sizeof s_voice))   // portal-selected voice
        strlcpy(s_voice, TTS_VOICE, sizeof s_voice);                // default if unset

    s_cancel   = false;
    s_audio_rx = 0;
    s_state    = TTS_IDLE;
    xStreamBufferReset(s_ring);                     // ring empty here (drain task idle) → safe
    xEventGroupClearBits(s_eg, BIT_CONNECTED | BIT_TERMINATED | BIT_WSERR | BIT_CANCEL);

    // P-b CONCURRENT-TLS PROBE: this runs from h_text_delta while the AG-UI SSE TLS is still open, so
    // these numbers are the two-up internal-RAM floor — the gate for whether streaming is viable here.
    ESP_LOGI(TAG, "open: internal free %u largest %u (SSE active)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    esp_websocket_client_config_t wcfg = {
        .uri = TTS_ENDPOINT,
        .buffer_size = WS_BUFFER_BYTES,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
    };
    s_ws = esp_websocket_client_init(&wcfg);
    if (!s_ws) { ESP_LOGE(TAG, "WSS init failed (internal RAM?) → caller falls back to batch"); xSemaphoreGive(s_lock); return ESP_FAIL; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) { ws_teardown(); xSemaphoreGive(s_lock); return ESP_FAIL; }

    // Bounded: open() runs inside the SSE read (h_text_delta), so this wait stalls the AG-UI reply.
    // 5 s covers a normal ~1 s handshake; a dead endpoint degrades to batch instead of a long freeze.
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_CONNECTED | BIT_WSERR | BIT_CANCEL,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(b & BIT_CONNECTED)) { ESP_LOGW(TAG, "tts connect failed/timeout → batch"); ws_teardown(); xSemaphoreGive(s_lock); return ESP_FAIL; }

    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "api_key", s_api_key);
    cJSON_AddStringToObject(c, "stream_id", TTS_STREAM_ID);
    cJSON_AddStringToObject(c, "model", TTS_MODEL);
    cJSON_AddStringToObject(c, "language", "en");
    cJSON_AddStringToObject(c, "voice", s_voice);
    cJSON_AddStringToObject(c, "audio_format", "pcm_s16le");
    cJSON_AddNumberToObject(c, "sample_rate", TTS_SR);
    if (send_json_locked(c) != ESP_OK) { ESP_LOGW(TAG, "config send failed → batch"); ws_teardown(); xSemaphoreGive(s_lock); return ESP_FAIL; }

    s_state = TTS_OPEN;
    ESP_LOGI(TAG, "tts stream open");
    return ESP_OK;                                  // s_lock held until wait_drained()
}

esp_err_t soniox_tts_feed(const char *text)
{
    if (s_state != TTS_OPEN || !text || !text[0]) return ESP_ERR_INVALID_STATE;
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "text", text);
    cJSON_AddBoolToObject(t, "text_end", false);
    cJSON_AddStringToObject(t, "stream_id", TTS_STREAM_ID);
    return send_json_locked(t);
}

esp_err_t soniox_tts_finish(void)
{
    if (s_state != TTS_OPEN) return ESP_OK;         // not open / already finishing → no-op
    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject(t, "text_end", true);
    cJSON_AddStringToObject(t, "stream_id", TTS_STREAM_ID);
    esp_err_t r = send_json_locked(t);
    s_state = TTS_FINISHING;
    return r;
}

// Block until the stream finishes (terminated + ring drained), errors, or is cancelled; then tear down
// the WSS and release s_lock. Must be called exactly once per successful open(), on EVERY exit path.
// `stall_ms` caps the time with NO new audio (a real stall) — NOT total wall-clock: Soniox streams a
// long reply's audio at ~playback rate (ring back-pressure), so {terminated} can be minutes away and a
// fixed total timeout would cut a long reply off mid-sentence. As long as audio keeps flowing, we wait.
esp_err_t soniox_tts_wait_drained(uint32_t stall_ms)
{
    if (s_state == TTS_IDLE) return ESP_OK;         // nothing open

    for (;;) {
        uint32_t rx_before = s_audio_rx;
        EventBits_t b = xEventGroupWaitBits(s_eg, BIT_TERMINATED | BIT_WSERR | BIT_CANCEL,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(stall_ms));
        if (b & BIT_CANCEL) { ESP_LOGI(TAG, "tts cancelled (barge-in)"); break; }
        if (b & BIT_WSERR)  { ESP_LOGW(TAG, "tts error before terminate"); break; }
        if (b & BIT_TERMINATED) {                   // all audio received → play out the queued ring
            int spins = 0;
            while (xStreamBufferBytesAvailable(s_ring) > 0 && spins++ < 600) vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(120));         // let the last DMA write flush
            break;
        }
        if (s_audio_rx != rx_before) continue;      // audio still arriving → keep waiting (long reply)
        ESP_LOGW(TAG, "tts drain stall (no audio for %u ms)", (unsigned)stall_ms);
        break;
    }

    ws_teardown();                                  // WSS gone → no more audio enters the ring
    if (s_cancel) {                                 // barge-in/error: drain task is discarding — empty the ring
        int spins = 0;
        while (xStreamBufferBytesAvailable(s_ring) > 0 && spins++ < 200) vTaskDelay(pdMS_TO_TICKS(2));
        s_cancel = false;
    }
    s_state = TTS_IDLE;
    ESP_LOGI(TAG, "tts closed: internal free %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

// Batch fallback (P-a): open the stream, feed the whole reply, finish, and drain — one blocking call.
esp_err_t soniox_tts_speak(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t e = soniox_tts_open();
    if (e != ESP_OK) return e;
    soniox_tts_feed(text);
    soniox_tts_finish();
    return soniox_tts_wait_drained(30000);          // 30 s no-audio stall cap (not total)
}

// PTT barge-in: stop an in-flight stream from another task. Takes NO lock (open()..wait_drained() hold
// s_lock) — only a flag + event bit. The drain task stops feeding the codec within ~1 chunk;
// wait_drained() wakes and tears down. No-op if nothing is streaming.
void soniox_tts_cancel(void)
{
    if (!s_eg) return;                         // not inited
    s_cancel = true;                           // drain task: discard; wait_drained(): drains the ring
    xEventGroupSetBits(s_eg, BIT_CANCEL);      // break the connect/terminate wait
}
