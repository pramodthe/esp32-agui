#include "speech_cfg.h"
#include "app_cfg.h"

#include <string.h>

static bool s_prov_cached;
static speech_provider_t s_prov = SPEECH_PROVIDER_SONIOX;

static speech_provider_t load_provider(void)
{
    char p[24];
    if (app_cfg_get(APP_CFG_SPEECH_PROVIDER, p, sizeof p)) {
        if (strcmp(p, SPEECH_PROVIDER_SONIOX_STR) == 0) return SPEECH_PROVIDER_SONIOX;
        if (strcmp(p, SPEECH_PROVIDER_DEEPGRAM_STR) == 0) return SPEECH_PROVIDER_DEEPGRAM;
    }
    // No explicit provider saved → keep Soniox (legacy soniox_key and fresh flash alike).
    return SPEECH_PROVIDER_SONIOX;
}

speech_provider_t speech_provider_get(void)
{
    if (!s_prov_cached) {
        s_prov = load_provider();
        s_prov_cached = true;
    }
    return s_prov;
}

void speech_cfg_invalidate(void)
{
    s_prov_cached = false;
}

const char *speech_provider_name(speech_provider_t p)
{
    return (p == SPEECH_PROVIDER_SONIOX) ? SPEECH_PROVIDER_SONIOX_STR
                                         : SPEECH_PROVIDER_DEEPGRAM_STR;
}

bool speech_cfg_get_key(char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    if (app_cfg_get(APP_CFG_SPEECH_KEY, buf, len)) return true;
    if (speech_provider_get() == SPEECH_PROVIDER_SONIOX)
        return app_cfg_get(APP_CFG_SONIOX_KEY, buf, len);
    return false;
}

bool speech_cfg_has_key(void)
{
    char tmp[APP_CFG_VAL_MAX];
    return speech_cfg_get_key(tmp, sizeof tmp);
}
