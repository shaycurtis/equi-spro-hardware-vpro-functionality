idf_component_register(SRCS "main.c" "device_id.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES power status_led sensors gnss telemetry app_update nvs_flash esp_timer)
