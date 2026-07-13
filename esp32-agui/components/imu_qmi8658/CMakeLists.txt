idf_component_register(
    SRCS "imu_qmi8658.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_timer esp32_s3_touch_amoled_1_8)
