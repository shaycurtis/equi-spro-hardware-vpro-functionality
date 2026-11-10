idf_component_register(SRCS "main.c" "device_id.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES cloud esp_app_format board power probe status_led sensors gnss telemetry ecg rfid provisioning storage recorder app_update nvs_flash esp_timer esp_driver_gpio esp_pm)
