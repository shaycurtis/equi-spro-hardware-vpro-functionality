idf_component_register(SRCS "provisioning.c"
                       INCLUDE_DIRS "include"
                       REQUIRES
                       PRIV_REQUIRES network_provisioning esp_wifi esp_netif esp_event esp_pm esp_timer nvs_flash
                                     power status_led telemetry)
