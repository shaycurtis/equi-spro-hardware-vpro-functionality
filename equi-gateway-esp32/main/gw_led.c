/*
 * Dev board RGB LED (WS2812 via Espressif's led_strip component, RMT).
 *
 *   blue, slow blink     Wi-Fi provisioning (join PROV_EQGW_xxxxxx, use the app)
 *   yellow, blinking     connecting to Wi-Fi
 *   cyan, blinking       Wi-Fi up: waiting for SNTP / fleet provisioning / MQTT
 *   green, steady (dim)  online; a short white flash for every burst received
 *   red, blinking        error (no claim certificate, provisioning rejected, no endpoint)
 *   white, fast          BOOT button held: re-entering provisioning
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "gw.h"

static led_strip_handle_t s_strip;
static volatile gw_led_state_t s_state = GW_LED_BOOT;
static volatile int s_pulse;

static void set(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    for (unsigned t = 0;; t++) {   /* 50 ms ticks */
        bool on1s = (t / 10) % 2 == 0, on250 = (t / 5) % 2 == 0;
        if (s_pulse > 0) {
            s_pulse--;
            set(40, 40, 40);
        } else {
            switch (s_state) {
            case GW_LED_BOOT:   set(10, 10, 10); break;
            case GW_LED_PROV:   on1s ? set(0, 0, 40) : set(0, 0, 0); break;
            case GW_LED_WIFI:   on250 ? set(30, 20, 0) : set(0, 0, 0); break;
            case GW_LED_CLOUD:  on250 ? set(0, 25, 25) : set(0, 0, 0); break;
            case GW_LED_ONLINE: set(0, 12, 0); break;
            case GW_LED_ERROR:  on1s ? set(40, 0, 0) : set(0, 0, 0); break;
            case GW_LED_BUTTON: (t % 2) ? set(40, 40, 40) : set(0, 0, 0); break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void gw_led_init(void)
{
    if (CONFIG_GW_LED_GPIO < 0) {
        return;
    }
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_GW_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW("led", "no LED on GPIO %d", CONFIG_GW_LED_GPIO);
        return;
    }
    xTaskCreate(led_task, "led", 2560, NULL, 3, NULL);
}

void gw_led_set(gw_led_state_t s)
{
    s_state = s;
}

void gw_led_pulse(void)
{
    s_pulse = 2;
}
