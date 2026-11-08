idf_component_register(SRCS "telemetry.c" "ecg_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt ecg sensors nvs_flash esp_timer)
