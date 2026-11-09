idf_component_register(SRCS "main.c" "device_id.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board power status_led sensors gnss telemetry ecg rfid provisioning app_update nvs_flash esp_timer esp_driver_gpio esp_pm)
