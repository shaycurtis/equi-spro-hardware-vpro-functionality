#include "sensors.h"
#include "sensors_priv.h"
#include "i2c_bus.h"
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

void sensors_shutdown(void)
{
    if (s_imu) {
        lsm6dsox_power_down();
    }
    /* SHTC3 is already asleep between readings; MAX17048 and the RTC stay on by design. */
}
