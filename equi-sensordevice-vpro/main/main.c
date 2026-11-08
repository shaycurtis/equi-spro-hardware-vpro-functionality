/*
 * Equimetrics sensor device - VPro firmware for the SPRO PCBA.
 *
 * Phase 1: power sequencing (power.h).
 * Phase 2a: slow sensors sampled every 30 s (sensors.h), GNSS parked in backup mode.
 * Phase 2b: each sample is broadcast as a BLE advert burst (telemetry.h).
 * Phase 2d: on-demand ECG over BLE (ecg.h, telemetry/ecg_service.h).
 * Sessions: each boot starts a session (ID = Unix time). At boot the RFID reader waits for
 * the horse's tag (both LEDs flash green); the tag becomes the session's HorseID. A short
 * press of the scan button (SW1) scans again; a different horse starts a new session.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "device_id.h"
#include "ecg.h"
#include "gnss.h"
#include "board.h"
#include "power.h"
#include "rfid.h"
#include "sensors.h"
#include "status_led.h"
#include "telemetry.h"

#define SAMPLE_PERIOD_S  30
#define ADV_BURST_MS     2000   /* 20 adverts at 100 ms; tune down once gateway reception is known */
#define RFID_TIMEOUT_S   120    /* how long to wait for a tag before carrying on without one */

static uint32_t s_dev_id;
static volatile uint32_t s_session;
static volatile uint64_t s_horse;      /* ISO 11784 packed, 0 = none */
static volatile bool s_scanning;

static const char *TAG = "main";

/* Temporary bring-up check: 3 s capture of the MAX30001 internal 1 Hz test signal at boot. */
#define ECG_BOOT_SELFTEST 0

#if ECG_BOOT_SELFTEST
static void selftest_block(const int16_t *s, size_t n, uint32_t first, void *ctx)
{
    int32_t mn = INT16_MAX, mx = INT16_MIN;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        mn = s[i] < mn ? s[i] : mn;
        mx = s[i] > mx ? s[i] : mx;
        sum += s[i];
    }
    ESP_LOGI("ecg_test", "[%5lu] n=%3u  min %6ld  max %6ld  mean %6lld  (%+.0f .. %+.0f uV)",
             (unsigned long)first, (unsigned)n, (long)mn, (long)mx, sum / (int64_t)n,
             mn * ECG_UV_PER_LSB, mx * ECG_UV_PER_LSB);
}
#endif

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

/* ------------------------------------------------------------ sessions */

static uint32_t new_session_id(void)
{
    uint32_t epoch;
    if (sensors_get_epoch(&epoch) == ESP_OK) {
        return epoch & 0x7FFFFFFF;
    }
    return esp_random() | 0x80000000u;   /* RTC has no time: random, flagged by bit 31 */
}

static void publish_identity(void)
{
    telemetry_ble_set_identity(s_dev_id, s_session, s_horse);
}

static void show_running_leds(void)
{
    status_led_set_mode(LED_IDX_STATUS, LED_MODE_PULSE, LED_RUNNING);   /* 5 s heartbeat */
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_OFF, LED_OFF);
    power_set_charge_led(true);          /* D2 back to the charge state (off unless on USB) */
}

static void on_tag(const rfid_tag_t *tag, void *ctx)
{
    if (tag) {
        uint64_t packed = rfid_tag_pack(tag);
        char str[16];
        rfid_tag_str(tag, str);
        if (s_horse && packed != s_horse) {
            s_session = new_session_id();
            ESP_LOGI(TAG, "different horse: new session %lu", (unsigned long)s_session);
        }
        s_horse = packed;
        ESP_LOGI(TAG, "HorseID %s (session %lu)", str, (unsigned long)s_session);
        publish_identity();
    } else {
        ESP_LOGW(TAG, "no tag scanned; continuing%s", s_horse ? " with the previous HorseID" : " without a HorseID");
    }
    s_scanning = false;
    show_running_leds();
}

static void start_rfid_scan(void)
{
    if (rfid_scanning()) {
        return;
    }
    power_set_charge_led(false);         /* both LEDs are the "scan a tag" prompt */
    status_led_set_mode(LED_IDX_STATUS, LED_MODE_BLINK, LED_GREEN);
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_BLINK, LED_GREEN);
    s_scanning = true;
    if (rfid_scan_start(RFID_TIMEOUT_S, on_tag, NULL) != ESP_OK) {
        s_scanning = false;
        show_running_leds();
    }
}

/* SW1: active low with a 10k pull-up. A short press starts a new tag scan. */
static void scan_button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = BIT64(BOARD_PIN_SCAN_BTN), .mode = GPIO_MODE_INPUT };
    gpio_config(&io);
    int low_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));      /* slow poll: the low-level wake-up covers sleep */
        if (gpio_get_level(BOARD_PIN_SCAN_BTN) == 0) {
            if ((low_ms += 100) == 200) {    /* debounced press */
                ESP_LOGI(TAG, "scan button pressed");
                start_rfid_scan();
            }
        } else {
            low_ms = 0;
        }
    }
}

/* ----------------------------------------------------------- telemetry */

static void build_telemetry(const sensor_sample_t *s, uint8_t seq, telemetry_t *t)
{
    power_charge_t c = power_charge_state();
    *t = (telemetry_t){
        .device_id = s_dev_id,
        .session = s_session,
        .seq = seq,
        .flags = (c != POWER_CHARGE_NO_USB ? TELEM_FLAG_USB : 0) |
                 (c == POWER_CHARGE_CHARGING ? TELEM_FLAG_CHARGING : 0) |
                 (c == POWER_CHARGE_DONE ? TELEM_FLAG_FULL : 0) |
                 (c == POWER_CHARGE_FAULT ? TELEM_FLAG_FAULT : 0) |
                 (s->rtc_ok && s->rtc_valid ? TELEM_FLAG_RTC_VALID : 0) |
                 (s_scanning ? TELEM_FLAG_RFID_SCAN : 0),
        .battery_soc256 = s->fg_ok ? (uint16_t)lroundf((s->batt_soc > 100.0f ? 100.0f : s->batt_soc) * 256.0f) : 0xFFFF,
        .battery_mv = s->fg_ok ? s->batt_mv : 0,
        .temp_c100 = s->th_ok ? (int16_t)lroundf(s->temp_c * 100.0f) : 0x7FFF,
        .humidity_half = s->th_ok ? (uint8_t)lroundf(s->rh * 2.0f) : 0xFF,
        .steps = s->imu_ok ? s->steps : 0xFFFFFFFF,
    };
}

static void sample_task(void *arg)
{
    /* Deadlines from esp_timer (real time): FreeRTOS ticks can lag during light sleep. */
    int64_t next_us = esp_timer_get_time();
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

        next_us += (int64_t)SAMPLE_PERIOD_S * 1000000;
        for (int64_t left; (left = next_us - esp_timer_get_time()) > 0;) {
            vTaskDelay(pdMS_TO_TICKS(left / 1000) + 1);
        }
    }
}

void app_main(void)
{
    power_early_init();
    ESP_ERROR_CHECK(status_led_init());
    power_boot();               /* returns only when the device should run */

    ESP_LOGI(TAG, "VPro running");
    confirm_ota_image();

    /* Dynamic frequency scaling + automatic light sleep whenever every task is idle.
     * Drivers that need a running clock (ECG SPI polling, RFID / GNSS UART) hold a
     * no-light-sleep lock while active. */
    esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true };
    esp_err_t pm_err = esp_pm_configure(&pm);
    ESP_LOGI(TAG, "power management: %s", pm_err == ESP_OK ? "light sleep enabled" : esp_err_to_name(pm_err));
    power_start();

    s_dev_id = strtoul(device_id_get(), NULL, 16) & 0xFFFFFF;
    gnss_standby_async();
    ESP_ERROR_CHECK(sensors_init());
    power_register_shutdown_cb(sensors_shutdown);

    s_session = new_session_id();
    ESP_LOGI(TAG, "session %lu%s", (unsigned long)s_session, (s_session & 0x80000000u) ? " (random: RTC not set)" : "");

    char name[24];
    snprintf(name, sizeof(name), "EQ-%s", device_id_get());
    ESP_ERROR_CHECK(telemetry_ble_init(name));
    publish_identity();
    power_register_shutdown_cb(telemetry_ble_stop);
    power_register_shutdown_cb(ecg_shutdown);
    power_register_shutdown_cb(rfid_shutdown);

    start_rfid_scan();                   /* "scan the horse's tag" */
    xTaskCreate(scan_button_task, "scan_btn", 2560, NULL, 3, NULL);

#if ECG_BOOT_SELFTEST
    ecg_request_t req = { .seconds = 3, .test_signal = true, .on_block = selftest_block };
    ESP_ERROR_CHECK(ecg_start(&req));
#endif

    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
}
