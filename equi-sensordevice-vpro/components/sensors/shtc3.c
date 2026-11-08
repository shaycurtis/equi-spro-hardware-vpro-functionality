/* Sensirion SHTC3 temperature / humidity. Kept in sleep mode between readings (~0.3 uA). */
#include "sensors_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#define ADDR          0x70
#define CMD_WAKEUP    0x3517
#define CMD_SLEEP     0xB098
#define CMD_ID        0xEFC8
#define CMD_MEAS_TF   0x7866   /* normal mode, T first, no clock stretching */
#define I2C_TIMEOUT   50

static const char *TAG = "shtc3";
static i2c_master_dev_handle_t s_dev;

static esp_err_t cmd(uint16_t c)
{
    uint8_t b[2] = { c >> 8, c & 0xff };
    return i2c_master_transmit(s_dev, b, 2, I2C_TIMEOUT);
}

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t wake(void)
{
    /* A sleeping SHTC3 may NACK the wake-up command while it wakes, so ignore the result. */
    cmd(CMD_WAKEUP);
    esp_rom_delay_us(300);          /* max 240 us; busy-wait, the 10 ms RTOS tick is too coarse */
    return ESP_OK;
}

static esp_err_t measure_once(float *temp_c, float *rh);

esp_err_t shtc3_read(float *temp_c, float *rh)
{
    esp_err_t err = measure_once(temp_c, rh);
    if (err != ESP_OK) {
        err = measure_once(temp_c, rh);
    }
    return err;
}

esp_err_t shtc3_init(void)
{
    if (!i2c_bus_probe(ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_add(ADDR, &s_dev), TAG, "add");
    ESP_RETURN_ON_ERROR(wake(), TAG, "wake");
    uint8_t id[3];
    ESP_RETURN_ON_ERROR(cmd(CMD_ID), TAG, "id cmd");
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, id, 3, I2C_TIMEOUT), TAG, "id read");
    uint16_t v = (uint16_t)(id[0] << 8 | id[1]);
    ESP_LOGI(TAG, "found, id 0x%04x%s", v, (v & 0x083F) == 0x0807 ? "" : " (unexpected)");
    return cmd(CMD_SLEEP);
}

static esp_err_t measure_once(float *temp_c, float *rh)
{
    wake();
    esp_err_t err = cmd(CMD_MEAS_TF);
    uint8_t d[6];
    if (err == ESP_OK) {
        /* Max 12.1 ms. With a 10 ms tick, vTaskDelay(n) may wait as little as (n-1) ticks,
         * so ask for 3 ticks to guarantee >= 20 ms. */
        vTaskDelay(pdMS_TO_TICKS(30));
        err = i2c_master_receive(s_dev, d, 6, I2C_TIMEOUT);
    }
    cmd(CMD_SLEEP);
    ESP_RETURN_ON_ERROR(err, TAG, "measure");
    ESP_RETURN_ON_FALSE(crc8(d, 2) == d[2] && crc8(d + 3, 2) == d[5], ESP_ERR_INVALID_CRC, TAG, "crc");

    uint16_t t = (uint16_t)(d[0] << 8 | d[1]);
    uint16_t h = (uint16_t)(d[3] << 8 | d[4]);
    *temp_c = -45.0f + 175.0f * t / 65536.0f;
    *rh = 100.0f * h / 65536.0f;
    return ESP_OK;
}
