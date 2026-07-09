// alarm_img — see include/alarm_img.h. Raw esp_partition access to the "alarmimg" data partition.
#include "alarm_img.h"

#include <string.h>
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "alarm_img";

#define PART_NAME    "alarmimg"
#define HDR_MAGIC    "ALM1"
#define HDR_VERSION  1
#define IMG_FORMAT   0           // 0 = RGB565 (LV_COLOR_16_SWAP byte order)

typedef struct __attribute__((packed)) {
    char     magic[4];           // "ALM1"
    uint16_t version;
    uint16_t w, h;
    uint16_t format;
    uint16_t reserved;
    uint32_t len;                // pixel bytes following the header
} alarm_img_hdr_t;

#define DATA_OFFSET  sizeof(alarm_img_hdr_t)

static const esp_partition_t *part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_NAME);
}

static bool read_hdr(const esp_partition_t *p, alarm_img_hdr_t *h)
{
    if (esp_partition_read(p, 0, h, sizeof *h) != ESP_OK) return false;
    return memcmp(h->magic, HDR_MAGIC, 4) == 0 && h->version == HDR_VERSION &&
           h->format == IMG_FORMAT && h->w == ALARM_IMG_W && h->h == ALARM_IMG_H &&
           h->len == ALARM_IMG_BYTES;
}

esp_err_t alarm_img_write(const uint8_t *data, size_t len)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    if (len != ALARM_IMG_BYTES) return ESP_ERR_INVALID_SIZE;
    const esp_partition_t *p = part();
    if (!p) { ESP_LOGW(TAG, "no '%s' partition (reflash the partition table)", PART_NAME); return ESP_ERR_NOT_FOUND; }

    size_t total = DATA_OFFSET + len;
    size_t erase = (total + 4095) & ~((size_t)4095);     // round up to the 4 KB flash erase block
    if (erase > p->size) return ESP_ERR_INVALID_SIZE;

    esp_err_t e = esp_partition_erase_range(p, 0, erase);
    if (e != ESP_OK) { ESP_LOGE(TAG, "erase: %s", esp_err_to_name(e)); return e; }

    if ((e = esp_partition_write(p, DATA_OFFSET, data, len)) != ESP_OK) {  // pixels first…
        ESP_LOGE(TAG, "write pixels: %s", esp_err_to_name(e)); return e;
    }
    alarm_img_hdr_t h = { .version = HDR_VERSION, .w = ALARM_IMG_W, .h = ALARM_IMG_H,
                          .format = IMG_FORMAT, .reserved = 0, .len = ALARM_IMG_BYTES };
    memcpy(h.magic, HDR_MAGIC, 4);
    if ((e = esp_partition_write(p, 0, &h, sizeof h)) != ESP_OK) {         // …header last = commit marker
        ESP_LOGE(TAG, "write hdr: %s", esp_err_to_name(e)); return e;
    }
    ESP_LOGI(TAG, "stored alarm image (%ux%u, %u B)", ALARM_IMG_W, ALARM_IMG_H, (unsigned)len);
    return ESP_OK;
}

bool alarm_img_present(void)
{
    const esp_partition_t *p = part();
    alarm_img_hdr_t h;
    return p && read_hdr(p, &h);
}

esp_err_t alarm_img_load(uint8_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    const esp_partition_t *p = part();
    alarm_img_hdr_t h;
    if (!p || !read_hdr(p, &h)) return ESP_ERR_NOT_FOUND;

    uint8_t *buf = heap_caps_malloc(ALARM_IMG_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_partition_read(p, DATA_OFFSET, buf, ALARM_IMG_BYTES);
    if (e != ESP_OK) { heap_caps_free(buf); return e; }
    *out = buf;
    return ESP_OK;
}

void alarm_img_free(uint8_t *buf) { if (buf) heap_caps_free(buf); }

esp_err_t alarm_img_clear(void)
{
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    return esp_partition_erase_range(p, 0, 4096);        // wipe the header block → present() == false
}
