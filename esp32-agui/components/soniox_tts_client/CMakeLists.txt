idf_component_register(
    SRCS "soniox_tts_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls json mbedtls app_cfg)
