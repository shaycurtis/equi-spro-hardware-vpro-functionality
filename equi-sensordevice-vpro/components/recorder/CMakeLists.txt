idf_component_register(SRCS "recorder.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ecg sensors storage esp_psram esp_timer)
