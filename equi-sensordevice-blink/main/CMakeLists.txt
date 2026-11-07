idf_component_register(SRCS "blink_main.c" "ws2812.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_rmt)
