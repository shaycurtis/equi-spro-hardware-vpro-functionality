idf_component_register(SRCS "rfid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board esp_driver_uart esp_driver_gpio esp_timer)
