idf_component_register(
    SRCS "deepgram_tts.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls json app_cfg speech_cfg)
