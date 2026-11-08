idf_component_register(SRCS "gnss.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_pm board esp_driver_uart esp_driver_gpio)
