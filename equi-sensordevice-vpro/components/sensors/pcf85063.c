/* NXP PCF85063A RTC, coin-cell backed. Read-only for now (time is set in a later phase). */
#include "sensors_priv.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"

#define ADDR         0x51
#define REG_SECONDS  0x04   /* bit7 = OS (oscillator stopped: time not valid) */

static const char *TAG = "pcf85063";
static i2c_master_dev_handle_t s_dev;

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }

esp_err_t pcf85063_init(void)
{
    if (!i2c_bus_probe(ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_add(ADDR, &s_dev), TAG, "add");
    ESP_LOGI(TAG, "found");
    return ESP_OK;
}

esp_err_t pcf85063_read(struct tm *t, bool *valid)
{
    uint8_t r[7];   /* seconds, minutes, hours, days, weekdays, months, years */
    ESP_RETURN_ON_ERROR(i2c_reg_read(s_dev, REG_SECONDS, r, sizeof(r)), TAG, "read");
    *valid = !(r[0] & 0x80);
    *t = (struct tm){
        .tm_sec  = bcd(r[0] & 0x7f),
        .tm_min  = bcd(r[1] & 0x7f),
        .tm_hour = bcd(r[2] & 0x3f),    /* 24 h mode (default) */
        .tm_mday = bcd(r[3] & 0x3f),
        .tm_wday = r[4] & 0x07,
        .tm_mon  = bcd(r[5] & 0x1f) - 1,
        .tm_year = bcd(r[6]) + 100,     /* 2000-2099 */
    };
    return ESP_OK;
}
