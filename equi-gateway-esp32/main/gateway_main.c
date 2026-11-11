/*
 * Equimetrics BLE gateway (ESP32-S3 dev board): SPRO telemetry adverts -> AWS IoT Core.
 * Replaces the PC gateway (gateway/ + gateway-receiver/) for the postBleData path. See ../README.md.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "gw.h"

static const char *TAG = "gw";

#define HOLD_TO_REPROVISION_MS 3000

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GW_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        held_ms = gpio_get_level(CONFIG_GW_BUTTON_GPIO) == 0 ? held_ms + 50 : 0;
        if (held_ms == 1000) {
            ESP_LOGI(TAG, "button: keep holding to re-enter Wi-Fi provisioning");
        }
        if (held_ms >= HOLD_TO_REPROVISION_MS) {
            gw_led_set(GW_LED_BUTTON);
            vTaskDelay(pdMS_TO_TICKS(700));
            gw_wifi_reprovision_and_restart();
        }
    }
}

static void status_loop(void)
{
    int64_t next_log = 0;
    for (;;) {
        bool wifi = gw_wifi_connected(), mqtt = gw_cloud_connected();
        gw_led_state_t led = !wifi ? GW_LED_WIFI : mqtt ? GW_LED_ONLINE : GW_LED_CLOUD;
        const char *st = gw_cloud_state();
        if (wifi && !mqtt && (st[0] == 'n' || strstr(st, "failed"))) {   /* "no ...", "... failed" */
            led = GW_LED_ERROR;
        }
        gw_led_set(led);

        int64_t now = esp_timer_get_time();
        if (now >= next_log) {
            next_log = now + 30 * 1000000LL;
            ESP_LOGI(TAG, "status: wifi %s (%d dBm), time %s, cloud %s, thing %s | devices heard 5 min: %d | "
                     "bursts %lu, published %lu, queued %d, dropped %lu | heap %lu",
                     wifi ? "up" : "down", wifi ? gw_wifi_rssi() : 0, gw_time_synced() ? "ok" : "not set", st,
                     gw_cloud_thing(), gw_ble_devices_heard(300), (unsigned long)gw_ble_bursts(),
                     (unsigned long)gw_cloud_published(), gw_queue_count(), (unsigned long)gw_queue_dropped(),
                     (unsigned long)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    char serial[13];
    gw_serial(serial);
    ESP_LOGI(TAG, "Equimetrics BLE gateway %s, serial %s", esp_app_get_description()->version, serial);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed: erasing (Wi-Fi and AWS identity are lost)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gw_led_init();
    gw_queue_init();
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);

    gw_wifi_start();   /* blocks in SoftAP provisioning until a network is set */
    gw_ble_start();    /* scanning buffers bursts even while offline */
    gw_cloud_start();
    status_loop();
}
