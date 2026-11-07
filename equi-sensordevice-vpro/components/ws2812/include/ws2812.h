#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

/* Minimal WS2812B(-2020) driver on the ESP32-S3 RMT peripheral. */

esp_err_t ws2812_init(gpio_num_t gpio, uint32_t led_count);

/* Set one pixel in the local buffer (call ws2812_show() to send). */
void ws2812_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/* Clear all pixels in the local buffer. */
void ws2812_clear(void);

/* Transmit the buffer to the LED chain and wait for completion. */
esp_err_t ws2812_show(void);
