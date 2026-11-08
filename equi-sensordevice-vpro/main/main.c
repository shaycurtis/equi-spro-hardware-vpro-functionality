/*
 * Equimetrics sensor device - VPro firmware for the SPRO PCBA.
 *
 * Phase 1: power sequencing (power.h).
 * Phase 2a: slow sensors sampled every 30 s (sensors.h), GNSS parked in backup mode.
 * Next: BLE advertising of each sample (2b).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "device_id.h"
#include "gnss.h"
#include "power.h"
#include "sensors.h"
#include "status_led.h"

#define SAMPLE_PERIOD_S  30

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

static const char *charge_str(power_charge_t c)
{
    static const char *n[] = { "battery", "charging", "full", "fault" };
    return n[c];
}

static void log_sample(const sensor_sample_t *s)
{
    char batt[48] = "batt n/a", th[32] = "T/RH n/a", steps[24] = "steps n/a", rtc[96] = "rtc n/a";
    if (s->fg_ok) {
        snprintf(batt, sizeof(batt), "batt %u mV %.1f%% %+.1f%%/h", s->batt_mv, s->batt_soc, s->batt_rate);
    }
    if (s->th_ok) {
        snprintf(th, sizeof(th), "%.2f C %.1f%%RH", s->temp_c, s->rh);
    }
    if (s->imu_ok) {
        snprintf(steps, sizeof(steps), "steps %lu", (unsigned long)s->steps);
    }
    if (s->rtc_ok) {
        const struct tm *t = &s->rtc_time;
        snprintf(rtc, sizeof(rtc), "rtc 20%02d-%02d-%02d %02d:%02d:%02d%s", t->tm_year - 100, t->tm_mon + 1,
                 t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, s->rtc_valid ? "" : " (INVALID)");
    }
    ESP_LOGI(TAG, "[%s] %s | %s | %s | %s | %s", device_id_get(), batt, th, steps,
             charge_str(power_charge_state()), rtc);
}

static void sample_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        sensor_sample_t s;
        sensors_read(&s);
        log_sample(&s);
        /* Phase 2b: build + advertise the BLE payload from `s` here. */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_S * 1000));
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

    device_id_get();
    gnss_standby_async();
    ESP_ERROR_CHECK(sensors_init());
    power_register_shutdown_cb(sensors_shutdown);

    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
}
