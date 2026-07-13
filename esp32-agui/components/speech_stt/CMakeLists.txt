idf_component_register(
    SRCS "speech_stt.c"
    INCLUDE_DIRS "include"
    REQUIRES speech_cfg soniox_client deepgram_stt app_cfg)
