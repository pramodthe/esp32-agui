#include "speech_stt.h"
#include "speech_cfg.h"
#include "app_cfg.h"
#include "soniox_client.h"
#include "deepgram_stt.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "speech_stt";
static speech_provider_t s_inited_for = (speech_provider_t)-1;

static void teardown_backend(speech_provider_t p)
{
    if (p == SPEECH_PROVIDER_SONIOX) {
        soniox_client_deinit();
    } else if (p == SPEECH_PROVIDER_DEEPGRAM) {
        deepgram_stt_deinit();
    }
}

static esp_err_t ensure_backend(void)
{
    speech_provider_t p = speech_provider_get();
    if (s_inited_for == p) return ESP_OK;

    // Tear down the previous backend's mic before opening another ES8311 handle
    // on the shared I2S-RX (bsp_audio_codec_microphone_init mints a new instance).
    if (s_inited_for == SPEECH_PROVIDER_SONIOX || s_inited_for == SPEECH_PROVIDER_DEEPGRAM) {
        ESP_LOGI(TAG, "switching STT %s → %s",
                 speech_provider_name(s_inited_for), speech_provider_name(p));
        teardown_backend(s_inited_for);
        s_inited_for = (speech_provider_t)-1;
    }

    esp_err_t e = (p == SPEECH_PROVIDER_SONIOX) ? soniox_client_init() : deepgram_stt_init();
    if (e == ESP_OK) {
        s_inited_for = p;
        ESP_LOGI(TAG, "backend ready: %s", speech_provider_name(p));
    }
    return e;
}

esp_err_t speech_stt_init(void) { return ensure_backend(); }

esp_err_t speech_stt_mic_stop(void)
{
    if (ensure_backend() != ESP_OK) return ESP_FAIL;
    return (speech_provider_get() == SPEECH_PROVIDER_SONIOX)
               ? soniox_client_mic_stop() : deepgram_stt_mic_stop();
}

esp_err_t speech_stt_mic_start(void)
{
    if (ensure_backend() != ESP_OK) return ESP_FAIL;
    return (speech_provider_get() == SPEECH_PROVIDER_SONIOX)
               ? soniox_client_mic_start() : deepgram_stt_mic_start();
}

esp_err_t speech_stt_session_start(const speech_stt_cfg_t *cfg,
                                   speech_stt_partial_cb on_partial,
                                   speech_stt_turn_cb on_turn, void *ctx)
{
    if (ensure_backend() != ESP_OK) return ESP_FAIL;

    char key[APP_CFG_VAL_MAX];
    const char *api_key = (cfg && cfg->api_key) ? cfg->api_key : NULL;
    if (!api_key) {
        if (!speech_cfg_get_key(key, sizeof key)) {
            ESP_LOGE(TAG, "no speech API key — set via portal");
            return ESP_ERR_NOT_FOUND;
        }
        api_key = key;
    }

    speech_provider_t p = speech_provider_get();
    ESP_LOGI(TAG, "session_start provider=%s", speech_provider_name(p));
    if (p == SPEECH_PROVIDER_SONIOX) {
        soniox_cfg_t sc = {
            .endpoint = cfg ? cfg->endpoint : NULL,
            .api_key = api_key,
            .model = cfg ? cfg->model : NULL,
            .sample_rate = cfg ? cfg->sample_rate : 0,
        };
        return soniox_session_start(&sc, on_partial, on_turn, ctx);
    }
    deepgram_stt_cfg_t dc = {
        .endpoint = cfg ? cfg->endpoint : NULL,
        .api_key = api_key,
        .model = cfg ? cfg->model : NULL,
        .sample_rate = cfg ? cfg->sample_rate : 0,
    };
    return deepgram_stt_session_start(&dc, on_partial, on_turn, ctx);
}

esp_err_t speech_stt_session_finalize(void)
{
    return (speech_provider_get() == SPEECH_PROVIDER_SONIOX)
               ? soniox_session_finalize() : deepgram_stt_session_finalize();
}

void speech_stt_session_stop(void)
{
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX) soniox_session_stop();
    else deepgram_stt_session_stop();
}

bool speech_stt_session_active(void)
{
    return (speech_provider_get() == SPEECH_PROVIDER_SONIOX)
               ? soniox_session_active() : deepgram_stt_session_active();
}

const char *speech_stt_last_error(void)
{
    return (speech_provider_get() == SPEECH_PROVIDER_SONIOX)
               ? soniox_last_error() : deepgram_stt_last_error();
}
