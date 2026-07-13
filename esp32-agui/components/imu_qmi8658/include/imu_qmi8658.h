// QMI8658 6-axis IMU on the Waveshare ESP32-S3-Touch-AMOLED-1.8 shared BSP I2C bus.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t imu_qmi8658_init(void);
bool      imu_qmi8658_ok(void);

// Smoothed tilt in degrees (roll around X, pitch around Y). 0 when unavailable.
void  imu_qmi8658_get_tilt(float *roll_deg, float *pitch_deg);

// Recent peak |Δaccel| magnitude (g). Spike when shaken.
float imu_qmi8658_shake_intensity(void);

// Compact orientation label for ambient context: "upright" | "flat" | "tilted" | "unknown".
const char *imu_qmi8658_orientation(void);

#ifdef __cplusplus
}
#endif
