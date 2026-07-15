idf_component_register(
    SRCS "chat_ui.c" "face_engine.c" "companion_pages.c"
    INCLUDE_DIRS "include" "."
    REQUIRES json lvgl esp32_s3_touch_amoled_1_8 device_tools alarm_img esp_timer
             esp_http_client esp_jpeg app_cfg imu_qmi8658 mbedtls)
