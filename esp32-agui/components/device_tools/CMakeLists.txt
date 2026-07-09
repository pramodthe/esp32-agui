idf_component_register(
    SRCS "device_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_driver_i2c esp_timer esp32_s3_touch_amoled_1_8 app_cfg)
# esp_driver_i2c + the BSP (bsp_i2c_get_handle) for AXP2101 battery reads on the shared I2C bus.
# P5/P7 add later: QMI8658 motion (device_motion) and PCF85063 RTC alarm (set_alarm tool).
