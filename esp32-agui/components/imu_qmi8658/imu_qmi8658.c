// Minimal QMI8658 driver for Waveshare ESP32-S3-Touch-AMOLED-1.8 (shared BSP I2C).
#include "imu_qmi8658.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "imu_qmi8658";

#define QMI_WHO_AM_I   0x00
#define QMI_CTRL1      0x02
#define QMI_CTRL2      0x03
#define QMI_CTRL3      0x04
#define QMI_CTRL5      0x06
#define QMI_CTRL7      0x08
#define QMI_AX_L       0x35
#define QMI_RESET      0x60
#define QMI_WHO_VAL    0x05
#define QMI_ADDR_A     0x6A
#define QMI_ADDR_B     0x6B

// ±4 g full-scale → ~8192 LSB/g (approx for QMI8658 ±4g)
#define ACC_LSB_PER_G  8192.0f

static i2c_master_dev_handle_t s_dev;
static bool  s_ok;
static float s_roll, s_pitch;          // smoothed degrees
static float s_shake;                  // decayed intensity
static float s_ax0, s_ay0, s_az0;      // still calibration baseline
static bool  s_calibrated;
static int   s_still_n;
static char  s_orient[16] = "unknown";

static esp_err_t qmi_rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t qmi_wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static bool probe_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) return false;
    uint8_t who = 0;
    uint8_t reg = QMI_WHO_AM_I;
    esp_err_t e = i2c_master_transmit_receive(dev, &reg, 1, &who, 1, 50);
    if (e != ESP_OK || who != QMI_WHO_VAL) {
        i2c_master_bus_rm_device(dev);
        return false;
    }
    s_dev = dev;
    ESP_LOGI(TAG, "QMI8658 at 0x%02X (WHO_AM_I=0x%02X)", addr, who);
    return true;
}

static bool configure(void)
{
    // Soft reset then bring up accel+gyro at ~125 Hz.
    qmi_wr(QMI_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (qmi_wr(QMI_CTRL1, 0x40) != ESP_OK) return false;          // address auto-increment
    if (qmi_wr(QMI_CTRL2, 0x16) != ESP_OK) return false;          // ±4g, aODR ~125 Hz
    if (qmi_wr(QMI_CTRL3, 0x56) != ESP_OK) return false;          // ±512 dps, gODR ~125 Hz
    if (qmi_wr(QMI_CTRL5, 0x11) != ESP_OK) return false;          // light LPF
    if (qmi_wr(QMI_CTRL7, 0x03) != ESP_OK) return false;          // aEN | gEN
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

static bool read_accel_g(float *ax, float *ay, float *az)
{
    uint8_t raw[6];
    if (qmi_rd(QMI_AX_L, raw, 6) != ESP_OK) return false;
    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);
    *ax = (float)x / ACC_LSB_PER_G;
    *ay = (float)y / ACC_LSB_PER_G;
    *az = (float)z / ACC_LSB_PER_G;
    return true;
}

static void update_orient(float ax, float ay, float az)
{
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag < 0.3f) {
        strlcpy(s_orient, "unknown", sizeof s_orient);
        return;
    }
    float nx = ax / mag, ny = ay / mag, nz = az / mag;
    if (fabsf(nz) > 0.85f) strlcpy(s_orient, "flat", sizeof s_orient);
    else if (fabsf(ny) > 0.7f || fabsf(nx) > 0.7f) strlcpy(s_orient, "upright", sizeof s_orient);
    else strlcpy(s_orient, "tilted", sizeof s_orient);
}

static void imu_task(void *arg)
{
    (void)arg;
    float prev_ax = 0, prev_ay = 0, prev_az = 1;
    bool have_prev = false;
    for (;;) {
        float ax, ay, az;
        if (s_ok && read_accel_g(&ax, &ay, &az)) {
            // Soft EMA for tilt (gravity-dominant).
            float roll  = atan2f(ay, az) * (180.0f / (float)M_PI);
            float pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * (180.0f / (float)M_PI);
            s_roll  = s_roll  * 0.85f + roll  * 0.15f;
            s_pitch = s_pitch * 0.85f + pitch * 0.15f;
            update_orient(ax, ay, az);

            if (!s_calibrated) {
                float j = fabsf(ax - prev_ax) + fabsf(ay - prev_ay) + fabsf(az - prev_az);
                if (have_prev && j < 0.05f) {
                    if (++s_still_n >= 20) {   // ~800 ms still
                        s_ax0 = ax; s_ay0 = ay; s_az0 = az;
                        s_calibrated = true;
                        ESP_LOGI(TAG, "calibrated (still)");
                    }
                } else {
                    s_still_n = 0;
                }
            }

            if (have_prev) {
                float d = fabsf(ax - prev_ax) + fabsf(ay - prev_ay) + fabsf(az - prev_az);
                if (d > s_shake) s_shake = d;
                else s_shake *= 0.92f;
            }
            prev_ax = ax; prev_ay = ay; prev_az = az;
            have_prev = true;
        }
        vTaskDelay(pdMS_TO_TICKS(40));   // ~25 Hz
    }
}

esp_err_t imu_qmi8658_init(void)
{
    if (s_ok) return ESP_OK;
    if (bsp_i2c_init() != ESP_OK) return ESP_FAIL;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_FAIL;

    if (!probe_addr(bus, QMI_ADDR_A) && !probe_addr(bus, QMI_ADDR_B)) {
        ESP_LOGW(TAG, "QMI8658 not found on 0x6A/0x6B");
        return ESP_ERR_NOT_FOUND;
    }
    if (!configure()) {
        ESP_LOGE(TAG, "configure failed");
        return ESP_FAIL;
    }
    s_ok = true;
    xTaskCreate(imu_task, "imu_qmi", 3072, NULL, 4, NULL);
    return ESP_OK;
}

bool imu_qmi8658_ok(void) { return s_ok; }

void imu_qmi8658_get_tilt(float *roll_deg, float *pitch_deg)
{
    if (roll_deg)  *roll_deg  = s_ok ? s_roll  : 0.f;
    if (pitch_deg) *pitch_deg = s_ok ? s_pitch : 0.f;
}

float imu_qmi8658_shake_intensity(void) { return s_ok ? s_shake : 0.f; }

const char *imu_qmi8658_orientation(void) { return s_ok ? s_orient : "unknown"; }
