idf_component_register(
    SRCS "soniox_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls json esp_codec_dev
             esp32_s3_touch_amoled_1_8 app_cfg)
