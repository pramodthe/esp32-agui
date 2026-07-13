idf_component_register(
    SRCS "speech_tts.c"
    INCLUDE_DIRS "include"
    REQUIRES speech_cfg soniox_tts_client deepgram_tts)
