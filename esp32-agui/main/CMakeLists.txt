idf_component_register(
    SRCS "esp32_agui_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        net_prov
        app_cfg
        soniox_client
        soniox_tts_client
        agui_client
        device_tools
        chat_ui
        board_variant
        esp32_s3_touch_amoled_1_8
        esp_codec_dev
        espressif__button)
