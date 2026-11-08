/*
 * Equimetrics sensor device - VPro firmware for the SPRO PCBA.
 *
 * Phase 1: power sequencing (power.h).
 * Phase 2a: slow sensors sampled every 30 s (sensors.h), GNSS parked in backup mode.
 * Phase 2b: each sample is broadcast as a BLE advert burst (telemetry.h).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "telemetry.h"

#define SAMPLE_PERIOD_S  30
#define ADV_BURST_MS     2000   /* 20 adverts at 100 ms; tune down once gateway reception is known */

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

static void build_telemetry(const sensor_sample_t *s, uint8_t seq, telemetry_t *t)
{
    power_charge_t c = power_charge_state();
    *t = (telemetry_t){
        .device_id = strtoul(device_id_get(), NULL, 16) & 0xFFFFFF,
        .seq = seq,
        .flags = (c != POWER_CHARGE_NO_USB ? TELEM_FLAG_USB : 0) |
                 (c == POWER_CHARGE_CHARGING ? TELEM_FLAG_CHARGING : 0) |
                 (c == POWER_CHARGE_DONE ? TELEM_FLAG_FULL : 0) |
                 (c == POWER_CHARGE_FAULT ? TELEM_FLAG_FAULT : 0) |
                 (s->rtc_ok && s->rtc_valid ? TELEM_FLAG_RTC_VALID : 0),
        .battery_soc = s->fg_ok ? (uint8_t)(s->batt_soc > 100.0f ? 100 : s->batt_soc + 0.5f) : 0xFF,
        .battery_mv = s->fg_ok ? s->batt_mv : 0,
        .temp_c100 = s->th_ok ? (int16_t)lroundf(s->temp_c * 100.0f) : 0x7FFF,
        .humidity_half = s->th_ok ? (uint8_t)lroundf(s->rh * 2.0f) : 0xFF,
        .steps = s->imu_ok ? s->steps : 0xFFFFFFFF,
        .uptime_min = (uint16_t)(esp_timer_get_time() / 60000000LL),
    };
}

static void sample_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint8_t seq = 0;
    for (;;) {
        sensor_sample_t s;
        sensors_read(&s);
        log_sample(&s);

        telemetry_t t;
        uint8_t payload[TELEMETRY_PAYLOAD_LEN];
        build_telemetry(&s, seq, &t);
        telemetry_encode(&t, payload);
        esp_err_t err;
        for (int tries = 0; (err = telemetry_ble_burst(payload, ADV_BURST_MS)) == ESP_ERR_INVALID_STATE && tries < 10; tries++) {
            vTaskDelay(pdMS_TO_TICKS(200));   /* BLE host not synced yet (first sample) */
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "advert seq %u sent (%d ms burst)", seq, ADV_BURST_MS);
        } else {
            ESP_LOGW(TAG, "advert seq %u failed: %s", seq, esp_err_to_name(err));
        }
        seq++;

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

    char name[24];
    snprintf(name, sizeof(name), "EQ-%s", device_id_get());
    ESP_ERROR_CHECK(telemetry_ble_init(name));
    power_register_shutdown_cb(telemetry_ble_stop);

    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
}
