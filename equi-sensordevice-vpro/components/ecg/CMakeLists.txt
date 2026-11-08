idf_component_register(SRCS "ecg.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_pm board max30001 esp_driver_gpio esp_timer)
