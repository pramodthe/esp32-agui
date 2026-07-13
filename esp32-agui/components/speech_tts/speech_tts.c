#include "speech_tts.h"
#include "speech_cfg.h"
#include "soniox_tts_client.h"
#include "deepgram_tts.h"

#include "esp_log.h"

static const char *TAG = "speech_tts";
static tts_pcm_sink_t s_sink;
static speech_provider_t s_inited_for = (speech_provider_t)-1;

static void teardown_backend(speech_provider_t p)
{
    if (p == SPEECH_PROVIDER_SONIOX) soniox_tts_deinit();
    else if (p == SPEECH_PROVIDER_DEEPGRAM) deepgram_tts_deinit();
}

static esp_err_t ensure_backend(void)
{
    speech_provider_t p = speech_provider_get();
    if (s_inited_for == p) return ESP_OK;

    if (s_inited_for == SPEECH_PROVIDER_SONIOX || s_inited_for == SPEECH_PROVIDER_DEEPGRAM) {
        ESP_LOGI(TAG, "switching TTS %s → %s",
                 speech_provider_name(s_inited_for), speech_provider_name(p));
        teardown_backend(s_inited_for);
        s_inited_for = (speech_provider_t)-1;
    }

    esp_err_t e = (p == SPEECH_PROVIDER_SONIOX)
                      ? soniox_tts_init(s_sink)
                      : deepgram_tts_init(s_sink);
    if (e == ESP_OK) {
        s_inited_for = p;
        ESP_LOGI(TAG, "backend ready: %s", speech_provider_name(p));
    }
    return e;
}

esp_err_t speech_tts_init(tts_pcm_sink_t sink)
{
    s_sink = sink;
    return ensure_backend();
}

esp_err_t speech_tts_speak(const char *text)
{
    if (ensure_backend() != ESP_OK) return ESP_FAIL;
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX) return soniox_tts_speak(text);
    return deepgram_tts_speak(text);
}

esp_err_t speech_tts_open(void)
{
    if (ensure_backend() != ESP_OK) return ESP_FAIL;
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX) return soniox_tts_open();
    return deepgram_tts_open();
}

esp_err_t speech_tts_feed(const char *text)
{
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX) return soniox_tts_feed(text);
    return deepgram_tts_feed(text);
}

esp_err_t speech_tts_finish(void)
{
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX) return soniox_tts_finish();
    return deepgram_tts_finish();
}

esp_err_t speech_tts_wait_drained(uint32_t timeout_ms)
{
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX) return soniox_tts_wait_drained(timeout_ms);
    return deepgram_tts_wait_drained(timeout_ms);
}

void speech_tts_cancel(void)
{
    if (s_inited_for == SPEECH_PROVIDER_SONIOX) soniox_tts_cancel();
    else if (s_inited_for == SPEECH_PROVIDER_DEEPGRAM) deepgram_tts_cancel();
}
