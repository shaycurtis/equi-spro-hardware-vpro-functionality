idf_component_register(SRCS "status_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board
                       PRIV_REQUIRES ws2812 esp_timer)
