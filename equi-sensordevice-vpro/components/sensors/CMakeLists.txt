idf_component_register(SRCS "sensors.c" "max17048.c" "shtc3.c" "lsm6dsox.c" "posture.c" "pcf85063.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES i2c_bus board esp_driver_gpio esp_timer)
