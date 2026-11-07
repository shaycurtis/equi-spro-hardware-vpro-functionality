/*
 * SPRO board bring-up: blink the two WS2812B-2020 RGB LEDs.
 *
 * Board facts (from SPRO.PcbDoc netlist):
 *   GPIO1  (module pin 39) -> LED  : data in of D1, D1 DOUT -> D2 DIN (2 LEDs chained)
 *   GPIO18 (module pin 11) -> PMIC_EN : diode-OR'd into the TPS22810 load-switch enable.
 *          On battery, the board only stays powered while the power button is held
 *          unless firmware drives this high, so we latch it first thing.
 *   UART0 (GPIO43/44) -> CP2102N USB-UART on the magnetic connector (console, 115200).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "ws2812.h"

#define PIN_LED       GPIO_NUM_1
#define PIN_PMIC_EN   GPIO_NUM_18
#define LED_COUNT     2
#define BRIGHTNESS    32   /* of 255: LEDs run from 3.3 V, keep current modest */

static const char *TAG = "blink";

static void power_latch_on(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_PMIC_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_PMIC_EN, 1);
}

void app_main(void)
{
    power_latch_on();
    ESP_LOGI(TAG, "Power latch (PMIC_EN/GPIO18) asserted");

    ESP_ERROR_CHECK(ws2812_init(PIN_LED, LED_COUNT));
    ESP_LOGI(TAG, "WS2812 on GPIO%d, %d LEDs", PIN_LED, LED_COUNT);

    const uint8_t colours[][3] = {
        { BRIGHTNESS, 0, 0 },   /* red   */
        { 0, BRIGHTNESS, 0 },   /* green */
        { 0, 0, BRIGHTNESS },   /* blue  */
    };
    const char *names[] = { "red", "green", "blue" };

    for (uint32_t n = 0;; n++) {
        uint32_t c = n % 3;
        /* LED on: D1 shows colour c, D2 shows the next colour so both are verified. */
        ws2812_set(0, colours[c][0], colours[c][1], colours[c][2]);
        uint32_t c2 = (c + 1) % 3;
        ws2812_set(1, colours[c2][0], colours[c2][1], colours[c2][2]);
        ws2812_show();
        ESP_LOGI(TAG, "blink %lu: D1=%s D2=%s", (unsigned long)n, names[c], names[c2]);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* LED off */
        ws2812_clear();
        ws2812_show();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
