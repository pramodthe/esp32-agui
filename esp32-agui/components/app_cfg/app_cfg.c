#include "app_cfg.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#define NS "appcfg"
static const char *TAG = "app_cfg";

esp_err_t app_cfg_set(const char *key, const char *val)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (val && val[0]) {
        err = nvs_set_str(h, key, val);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "set %s (%s)", key, esp_err_to_name(err));
    return err;
}

bool app_cfg_get(const char *key, char *buf, size_t len)
{
    if (!key || !buf || len == 0) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = len;
    esp_err_t err = nvs_get_str(h, key, buf, &l);
    nvs_close(h);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "%s too long for %u-byte buffer (raise APP_CFG_VAL_MAX)", key, (unsigned)len);
        return false;
    }
    if (err != ESP_OK) return false;
    return buf[0] != '\0';
}

bool app_cfg_has(const char *key)
{
    char tmp[8];
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = 0;
    // Probe length only (passing NULL out-buffer returns required size).
    esp_err_t err = nvs_get_str(h, key, NULL, &l);
    nvs_close(h);
    (void)tmp;
    return err == ESP_OK && l > 1;   // l includes NUL; >1 means non-empty
}
