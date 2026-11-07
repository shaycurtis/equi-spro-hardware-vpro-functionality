/*
 * Equimetrics sensor device - VPro firmware for the SPRO PCBA.
 *
 * Phase 1: power sequencing (power.h). Sensors, BLE telemetry and SD/EDF logging
 * will register with power_register_shutdown_cb() so they stop cleanly on power-off.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "power.h"
#include "status_led.h"

static const char *TAG = "main";

static void confirm_ota_image(void)
{
    /* Bootloader rollback is enabled: an OTA image must confirm itself once it runs. */
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid");
    }
}

void app_main(void)
{
    power_early_init();
    ESP_ERROR_CHECK(status_led_init());
    power_boot();               /* returns only when the device should run */

    ESP_LOGI(TAG, "VPro running");
    confirm_ota_image();
    power_start();

    /* Phase 2: sensors + BLE telemetry start here. */
}
