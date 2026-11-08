/* MAX17048 1-cell fuel gauge (ModelGauge). Always powered from the battery. */
#include "sensors_priv.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"

#define ADDR        0x36
#define REG_VCELL   0x02   /* 78.125 uV/LSB */
#define REG_SOC     0x04   /* 1/256 %/LSB   */
#define REG_VERSION 0x08
#define REG_CRATE   0x16   /* signed, 0.208 %/h per LSB */

static const char *TAG = "max17048";
static i2c_master_dev_handle_t s_dev;

static esp_err_t read16(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(i2c_reg_read(s_dev, reg, b, 2), TAG, "read 0x%02x", reg);
    *v = (uint16_t)(b[0] << 8 | b[1]);
    return ESP_OK;
}

esp_err_t max17048_init(void)
{
    if (!i2c_bus_probe(ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_add(ADDR, &s_dev), TAG, "add");
    uint16_t ver;
    ESP_RETURN_ON_ERROR(read16(REG_VERSION, &ver), TAG, "version");
    ESP_LOGI(TAG, "found, version 0x%04x", ver);
    return ESP_OK;
}

esp_err_t max17048_read(uint16_t *mv, float *soc, float *rate)
{
    uint16_t vcell, s;
    uint16_t crate;
    ESP_RETURN_ON_ERROR(read16(REG_VCELL, &vcell), TAG, "vcell");
    ESP_RETURN_ON_ERROR(read16(REG_SOC, &s), TAG, "soc");
    ESP_RETURN_ON_ERROR(read16(REG_CRATE, &crate), TAG, "crate");
    *mv = (uint16_t)(((uint32_t)vcell * 5) / 64);   /* * 78.125 / 1000 */
    *soc = s / 256.0f;
    *rate = (int16_t)crate * 0.208f;
    return ESP_OK;
}
