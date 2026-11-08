idf_component_register(SRCS "rfid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_pm board esp_driver_uart esp_driver_gpio esp_timer)
