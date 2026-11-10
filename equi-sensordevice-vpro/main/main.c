/*
 * Equimetrics sensor device - VPro firmware for the SPRO PCBA.
 *
 * Phase 1: power sequencing (power.h).
 * Phase 2a: slow sensors sampled every 60 s (sensors.h), GNSS parked in backup mode.
 * Phase 2b: each sample is broadcast as a BLE advert burst (telemetry.h).
 * Phase 2d: on-demand ECG over BLE (ecg.h, telemetry/ecg_service.h).
 * Sessions: at boot the RFID reader waits for the horse's tag (both LEDs flash green). The
 * session (ID = Unix time) starts when the tag is read, or when the scan button (SW1) is
 * pressed during the wait (no tag: HorseID unknown). SD recording starts with the session.
 * Later, a short SW1 press scans again; a different horse starts a new session.
 */
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_mac.h"
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
#include "provisioning.h"
#include "storage.h"
#include "recorder.h"
#include "rfid.h"
#include "sensors.h"
#include "status_led.h"
#include "telemetry.h"
#include "cloud.h"
#include "esp_app_desc.h"

#define SAMPLE_PERIOD_S  60     /* matches the VPro LoRa device: one reading a minute */
#define ADV_BURST_MS     2000   /* 20 adverts at 100 ms; tune down once gateway reception is known */
#define RFID_TIMEOUT_S   120    /* how long to wait for a tag (no session starts without one or the override) */

static uint32_t s_dev_id;
static volatile uint64_t s_session;   /* Unix time in ms (as VPro), 0 = no session yet */
static volatile uint64_t s_horse;      /* ISO 11784 packed, 0 = none */
static volatile bool s_scanning;

static const char *TAG = "main";

/* Temporary bring-up check: 3 s capture of the MAX30001 internal 1 Hz test signal at boot. */
#define ECG_BOOT_SELFTEST 0
#define SD_BOOT_SPEEDTEST 0   /* bring-up: 1 MB write/read test on the SD card at boot */
#define CLOUD_HW_VERSION  "SPRO-PCBA"   /* hardware as reported to the backend (VPro sends its own) */

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
    uint8_t bpm, beats;
    ecg_hr_get(SAMPLE_PERIOD_S, &bpm, &beats);
    char hr[24] = "HR n/a";
    if (bpm) {
        snprintf(hr, sizeof(hr), "HR %u bpm (%u beats)", bpm, beats);
    }
    ESP_LOGI(TAG, "[%s] %s | %s | %s | %s | %s | %s", device_id_get(), batt, th, steps, hr,
             charge_str(power_charge_state()), rtc);
}

/* ------------------------------------------------------------ sessions */

static uint32_t now_epoch(void)
{
    uint32_t epoch;
    return sensors_get_epoch(&epoch) == ESP_OK ? epoch : 0;   /* 0 = RTC has no time */
}

/* Session ID = Unix time in milliseconds, as the VPro platform uses (the RTC has 1 s
 * resolution, so the last three digits are 000). */
static uint64_t new_session_id(void)
{
    uint32_t epoch = now_epoch();
    if (epoch) {
        return (uint64_t)epoch * 1000u;
    }
    return SESSION_RANDOM_BIT | esp_random();   /* RTC has no time: random, flagged by bit 52 */
}

static volatile int64_t s_identity_at_us;   /* when the current identity was published */

static void publish_identity(void)
{
    telemetry_ble_set_identity(s_dev_id, s_session, s_horse);
    s_identity_at_us = esp_timer_get_time();   /* not delivered until a gateway asks again */
}

/* A gateway counts as "connected" while it answered (scan request) within this window:
 * the last burst plus one missed one, so a single lost burst does not flip the LED. */
#define GATEWAY_SEEN_MS   ((SAMPLE_PERIOD_S * 2 + 5) * 1000)

static bool s_linked;

/* Ask gateways to connect (advert flag) when no trusted gateway has heard us lately. Right after
 * boot a known gateway gets one window to show up first, so it does not reconnect every boot. */
static bool gateway_wanted(void)
{
    if (telemetry_gateway_seen(GATEWAY_SEEN_MS)) {
        return false;
    }
    return !telemetry_has_trusted_gateway() || esp_timer_get_time() / 1000 > GATEWAY_SEEN_MS;
}

/* The identity (session + HorseID) travels in the scan response, so a trusted gateway's scan
 * request after it was published means that gateway has received it. */
static bool identity_delivered(void)
{
    return telemetry_gateway_last_seen_us() > s_identity_at_us;
}

/*
 * D1 while running:
 *   steady blue  - session not yet delivered to a trusted gateway ("not connected")
 *   green blink  - delivered, and a trusted gateway is hearing us
 *   blue blink   - delivered earlier, but no trusted gateway heard us for ~1 min (out of range)
 */
static void show_status_led(void)
{
    if (!identity_delivered()) {
        status_led_set_mode(LED_IDX_CHARGE, LED_MODE_SOLID, LED_BLUE);
    } else {
        status_led_set_mode(LED_IDX_STATUS, LED_MODE_PULSE,
                            telemetry_gateway_seen(GATEWAY_SEEN_MS) ? LED_LINKED : LED_RUNNING);
    }
}

static void show_running_leds(void)
{
    show_status_led();
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_OFF, LED_OFF);
    power_set_charge_led(true);          /* D2 back to the charge state (off unless on USB) */
}

static volatile bool s_override;   /* scan button pressed while waiting for a tag */

static char s_horse_str[16];   /* chip number of the current session, "" = none / unknown */

/* App-started recording (ECG control service). With continuous ECG on and a session running,
 * the session file already has it: live view only. Otherwise its own file, in the session's
 * folder (or one named after the recording if there is no session yet). */
static esp_err_t app_rec_start(uint64_t id, uint32_t seconds)
{
    const storage_config_t *cfg = storage_config();
    if (cfg->ecg_log && s_session) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    recorder_clip_cfg_t c = {
        .device_id = device_id_backend_get(),
        .session = s_session ? s_session : id,
        .id = id,
        .epoch = id > 1000000000000ULL ? (uint32_t)(id / 1000) : now_epoch(),
        .horse = s_session && s_horse_str[0] ? s_horse_str : NULL,
        .rate_hz = cfg->ecg_rate_hz,
    };
    esp_err_t err = recorder_clip_start(&c);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "app recording %llu started (%lu s, 0 = until stopped)", (unsigned long long)id,
                 (unsigned long)seconds);
    }
    return err;
}

static void app_rec_stop(bool keep)
{
    recorder_clip_stop(keep);
}

/* Start a session (tag read, or the scan-button override with horse == NULL). */
/* ------------------------------------------------------------ charging: backend */

static char s_fw_ver[40];
static char s_last_update[24];

static void cloud_read_status(cloud_status_t *st)
{
    sensor_sample_t s = { 0 };
    sensors_read(&s);
    st->battery_pct = s.batt_soc > 100 ? 100 : s.batt_soc < 0 ? 0 : (uint8_t)(s.batt_soc + 0.5f);
    st->temperature_c = s.temp_c;
    st->humidity_pct = s.rh;
    st->charging = power_charge_now() != POWER_CHARGE_DONE;
}

static void cloud_set_time(uint32_t epoch)
{
    sensors_set_time(epoch);   /* RTC + system clock */
}

/* Charge-only LEDs: D1 (the more visible one) shows the charge, D2 the Wi-Fi / upload state.
 * Orange = Wi-Fi / upload problem (purple is kept for OTA updates later). */
static void cloud_show(cloud_show_t st)
{
    switch (st) {
    case CLOUD_SHOW_CONNECTING:
        power_charge_only_claim_status_led(true);
        status_led_set_mode(LED_IDX_CHARGE, LED_MODE_BLINK, LED_BLUE);
        break;
    case CLOUD_SHOW_UPLOADING:
        power_charge_only_claim_status_led(true);
        status_led_set_mode(LED_IDX_CHARGE, LED_MODE_BREATHE, LED_BLUE);
        break;
    case CLOUD_SHOW_SYNCED:
        power_charge_only_claim_status_led(true);
        status_led_set_mode(LED_IDX_CHARGE, LED_MODE_SOLID, LED_BLUE);
        break;
    case CLOUD_SHOW_FAILED:
        power_charge_only_claim_status_led(true);
        status_led_set_mode(LED_IDX_CHARGE, LED_MODE_PULSE, LED_ORANGE);
        break;
    default:
        power_charge_only_claim_status_led(false);   /* back to the charge colour */
        break;
    }
}

/* Charge-only on USB: start the backend task (Wi-Fi to a saved network, time, telemetry). */
static void on_charge_only(void)
{
    device_id_get();           /* opens NVS (saved networks) */
    if (sensors_init() != ESP_OK) {
        ESP_LOGW(TAG, "sensors unavailable for the charging telemetry");
    }
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_fw_ver, sizeof(s_fw_ver), "%s", app->version);
    struct tm bt = { 0 };
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    if (strptime(built, "%b %d %Y %H:%M:%S", &bt)) {
        strftime(s_last_update, sizeof(s_last_update), "%Y-%m-%dT%H:%M:%SZ", &bt);   /* build time */
    } else {
        strlcpy(s_last_update, "1970-01-01T00:00:00Z", sizeof(s_last_update));
    }
    cloud_cfg_t c = {
        .device_id = device_id_backend_get(),   /* e.g. "50787DFFFE15B0B4": as the backend knows it */
        .hw_version = CLOUD_HW_VERSION,
        .fw_version = s_fw_ver,
        .last_update = s_last_update,
        .read_status = cloud_read_status,
        .set_time = cloud_set_time,
        .show = cloud_show,
    };
    ESP_LOGI(TAG, "charging: backend task for device %s", device_id_backend_get());
    cloud_charging_start(&c);
}

static void start_session(uint64_t packed, const char *horse)
{
    strlcpy(s_horse_str, horse ? horse : "", sizeof(s_horse_str));
    bool first = s_session == 0;
    uint32_t epoch = now_epoch();
    s_session = new_session_id();
    s_horse = packed;
    ESP_LOGI(TAG, "session %llu started, HorseID %s%s", (unsigned long long)s_session, horse ? horse : "Unknown",
             (s_session & SESSION_RANDOM_BIT) ? " (random ID: RTC not set)" : "");
    publish_identity();
    const storage_config_t *cfg = storage_config();
    if (storage_mounted() && (cfg->ecg_log || cfg->imu_log)) {
        if (first) {
            recorder_cfg_t rc = {
                .device_id = device_id_backend_get(), .session = s_session, .epoch = epoch, .horse = horse,
                .ecg_rate_hz = cfg->ecg_log ? cfg->ecg_rate_hz : 0,
                .imu_rate_hz = cfg->imu_log ? cfg->imu_rate_hz : 0,
            };
            if (recorder_start(&rc) != ESP_OK) {
                ESP_LOGW(TAG, "SD recording not started");
            }
        } else {
            recorder_new_session(s_session, epoch, horse);
        }
    }
}

static void on_tag(const rfid_tag_t *tag, void *ctx)
{
    if (tag) {
        uint64_t packed = rfid_tag_pack(tag);
        char str[16];
        rfid_tag_str(tag, str);
        if (!s_session || packed != s_horse) {
            start_session(packed, str);          /* first tag, or a different horse */
        } else {
            ESP_LOGI(TAG, "same horse %s: session %llu continues", str, (unsigned long long)s_session);
        }
    } else if (s_override) {
        if (!s_session) {
            start_session(0, NULL);              /* no tag available: HorseID unknown */
        } else {
            ESP_LOGI(TAG, "scan cancelled: session %llu continues", (unsigned long long)s_session);
        }
    } else {
        ESP_LOGW(TAG, "no tag scanned%s", s_session ? "; session continues" :
                 ": no session started (press the scan button to scan again)");
    }
    s_override = false;
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
#define REPROV_HOLD_MS  3000   /* both buttons held this long: back to Wi-Fi provisioning */

/* SW1 short press (released within 2 s, power button not touched): rescan the tag.
 * SW1 + power button held together for 3 s: Wi-Fi provisioning mode (add a network). */
static void scan_button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = BIT64(BOARD_PIN_SCAN_BTN), .mode = GPIO_MODE_INPUT };
    gpio_config(&io);
    int low_ms = 0, both_ms = 0;
    bool combo = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));      /* slow poll: the low-level wake-up covers sleep */
        if (power_shutting_down()) {
            vTaskDelay(portMAX_DELAY);           /* powering off: no more gestures */
        }
        bool scan = gpio_get_level(BOARD_PIN_SCAN_BTN) == 0;
        bool pwr = gpio_get_level(BOARD_PIN_PWR_BTN) == 1;
        power_set_combo(scan && pwr);
        if (scan) {
            low_ms += 100;
            if (pwr) {
                combo = true;
                if ((both_ms += 100) == 500) {
                    status_led_set(LED_WHITE);   /* "keep holding both" */
                }
                if (both_ms >= REPROV_HOLD_MS) {
                    prov_request_and_restart();
                }
            } else if (both_ms) {
                both_ms = 0;
                status_led_override_clear();
            }
            continue;
        }
        if (both_ms) {
            status_led_override_clear();
        }
        if (!combo && low_ms >= 200 && low_ms <= 2000) {
            if (s_scanning) {
                ESP_LOGI(TAG, "scan button pressed during the scan: override (no tag)");
                s_override = true;
                rfid_scan_cancel();          /* on_tag(NULL) then starts the session */
            } else {
                ESP_LOGI(TAG, "scan button pressed");
                start_rfid_scan();
            }
        }
        low_ms = both_ms = 0;
        combo = false;
    }
}

/* ----------------------------------------------------------- telemetry */

static void build_telemetry(const sensor_sample_t *s, uint8_t seq, telemetry_t *t)
{
    power_charge_t c = power_charge_state();
    *t = (telemetry_t){
        .device_id = s_dev_id,
        .session = (uint32_t)s_session,   /* only the low byte is sent */
        .seq = seq,
        .flags = (c != POWER_CHARGE_NO_USB ? TELEM_FLAG_USB : 0) |
                 (c == POWER_CHARGE_CHARGING ? TELEM_FLAG_CHARGING : 0) |
                 (c == POWER_CHARGE_DONE ? TELEM_FLAG_FULL : 0) |
                 (c == POWER_CHARGE_FAULT ? TELEM_FLAG_FAULT : 0) |
                 (s->rtc_ok && s->rtc_valid ? TELEM_FLAG_RTC_VALID : 0) |
                 (s_scanning ? TELEM_FLAG_RFID_SCAN : 0) |
                 (gateway_wanted() ? TELEM_FLAG_NO_GATEWAY : 0),
        .battery_soc256 = s->fg_ok ? (uint16_t)lroundf((s->batt_soc > 100.0f ? 100.0f : s->batt_soc) * 256.0f) : 0xFFFF,
        .battery_mv = s->fg_ok ? s->batt_mv : 0,
        .temp_c100 = s->th_ok ? (int16_t)lroundf(s->temp_c * 100.0f) : 0x7FFF,
        .humidity_half = s->th_ok ? (uint8_t)lroundf(s->rh * 2.0f) : 0xFF,
        .steps = s->imu_ok ? s->steps : 0xFFFFFFFF,
    };
    uint8_t beats;
    ecg_hr_get(SAMPLE_PERIOD_S, &t->heart_rate, &beats);
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
#ifdef CONFIG_PM_PROFILING
        if (seq % 2 == 0) {
            esp_pm_dump_locks(stdout);   /* power profiling: time per mode and per lock (cumulative) */
        }
#endif

        /* After the burst: green heartbeat if a gateway answered, blue if not. */
        uint32_t reqs0 = telemetry_scan_requests();
        vTaskDelay(pdMS_TO_TICKS(ADV_BURST_MS + 100));
        bool linked = telemetry_gateway_seen(GATEWAY_SEEN_MS);
        if (linked != s_linked) {
            s_linked = linked;
            ESP_LOGI(TAG, "gateway %s", linked ? "connected (scan requests received)" : "lost");
        }
        ESP_LOGI(TAG, "%lu gateway scan requests in the burst", (unsigned long)(telemetry_scan_requests() - reqs0));
        telemetry_log_scanners();
        if (!s_scanning) {
            show_status_led();
        }

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
    power_set_charge_only_hook(on_charge_only);   /* Wi-Fi + backend while charging */
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
    if (prov_required() || power_boot_prov_gesture()) {
        prov_run(s_dev_id);                /* locked until Wi-Fi is set up; restarts when done */
    }
    ESP_ERROR_CHECK(sensors_init());
    power_register_shutdown_cb(sensors_shutdown);
    if (storage_init() == ESP_OK) {        /* SD card + config.json */
        power_register_shutdown_cb(storage_shutdown);
#if SD_BOOT_SPEEDTEST
        storage_speed_test();
#endif
    }

    /* SD recording starts with the session; its hook runs before the card is unmounted. */
    power_register_shutdown_cb(recorder_shutdown);

    char name[24];
    /* "EQUI_" + the device ID, as VPro's BOARD_HOSTNAME_PREFIX + ID. The Equimetrics app only
     * connects to IDs registered in the backend (the devices record for "15B0B4"). */
    snprintf(name, sizeof(name), "EQUI_%s", device_id_backend_get());   /* the app needs the full ID */
    ESP_ERROR_CHECK(telemetry_ble_init(name));   /* identity is published when the session starts */
    telemetry_set_recording_hooks(app_rec_start, app_rec_stop);
    power_register_shutdown_cb(telemetry_ble_stop);
    power_register_shutdown_cb(ecg_shutdown);
    ESP_ERROR_CHECK(ecg_init());           /* continuous heart rate from the MAX30001 */
    power_register_shutdown_cb(rfid_shutdown);

    start_rfid_scan();                   /* "scan the horse's tag" */
    xTaskCreate(scan_button_task, "scan_btn", 2560, NULL, 3, NULL);

#if ECG_BOOT_SELFTEST
    ecg_request_t req = { .seconds = 3, .test_signal = true, .on_block = selftest_block };
    ESP_ERROR_CHECK(ecg_start(&req));
#endif

    xTaskCreate(sample_task, "sample", 4096, NULL, 5, NULL);
}
