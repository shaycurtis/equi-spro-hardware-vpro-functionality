#include "sensors.h"
#include "sensors_priv.h"
#include "i2c_bus.h"
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "sensors";
static bool s_fg, s_th, s_imu, s_rtc;

static bool try_init(const char *name, esp_err_t (*fn)(void))
{
    esp_err_t err = fn();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s not available: %s", name, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

esp_err_t sensors_init(void)
{
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    s_fg  = try_init("MAX17048", max17048_init);
    s_th  = try_init("SHTC3", shtc3_init);
    s_imu = try_init("LSM6DSOX", lsm6dsox_init);
    s_rtc = try_init("PCF85063A", pcf85063_init);

    /* System clock from the RTC, so log timestamps and session IDs are wall-clock time. */
    uint32_t epoch;
    if (sensors_get_epoch(&epoch) == ESP_OK) {
        struct timeval tv = { .tv_sec = epoch };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system time set from RTC: %lu", (unsigned long)epoch);
    } else {
        ESP_LOGW(TAG, "RTC has no time (lost power); the gateway will set it");
    }
    return ESP_OK;
}

esp_err_t sensors_get_epoch(uint32_t *epoch_utc)
{
    struct tm tm;
    bool valid = false;
    if (!s_rtc || pcf85063_read(&tm, &valid) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    /* After a real power loss the PCF85063A restarts at 2000-01-01, so a plausible date means
     * the time is still usable even if the oscillator-stop flag is set (a brief stop only
     * loses seconds). The flag is still reported as "RTC not valid", so the gateway re-syncs. */
    if (tm.tm_year < 124) {               /* before 2024: really lost */
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid) {
        ESP_LOGW(TAG, "RTC oscillator-stop flag set but date is plausible; using it");
    }
    tm.tm_isdst = 0;
    time_t t = mktime(&tm);              /* TZ is UTC (unset), so mktime == timegm */
    if (t < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    *epoch_utc = (uint32_t)t;
    return ESP_OK;
}

void sensors_read(sensor_sample_t *s)
{
    *s = (sensor_sample_t){ 0 };
    s->fg_ok  = s_fg  && max17048_read(&s->batt_mv, &s->batt_soc, &s->batt_rate) == ESP_OK;
    s->th_ok  = s_th  && shtc3_read(&s->temp_c, &s->rh) == ESP_OK;
    s->imu_ok = s_imu && lsm6dsox_read_steps(&s->steps) == ESP_OK;
    s->rtc_ok = s_rtc && pcf85063_read(&s->rtc_time, &s->rtc_valid) == ESP_OK;
}

esp_err_t sensors_set_time(uint32_t epoch_utc)
{
    if (!s_rtc) {
        return ESP_ERR_NOT_FOUND;
    }
    time_t t = (time_t)epoch_utc;
    struct tm tm;
    gmtime_r(&t, &tm);
    esp_err_t err = pcf85063_write(&tm);
    if (err == ESP_OK) {
        struct timeval tv = { .tv_sec = t };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "RTC + system time set to %04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    return err;
}

void sensors_shutdown(void)
{
    if (s_imu) {
        lsm6dsox_power_down();
    }
    /* SHTC3 is already asleep between readings; MAX17048 and the RTC stay on by design. */
}
