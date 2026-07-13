idf_component_register(
    SRCS "device_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_driver_i2c esp_timer esp32_s3_touch_amoled_1_8 app_cfg imu_qmi8658)
# AXP2101 battery + QMI8658 motion on the shared BSP I2C bus.
