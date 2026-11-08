/*
 * ST LSM6DSOX: accelerometer at 26 Hz in low-power mode with the embedded hardware
 * pedometer. The gyro stays powered down. (Later: 416 Hz IMU capture for SD logging.)
 */
#include "sensors_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"

#define ADDR                 0x6B
#define WHO_AM_I_VAL         0x6C

/* Main register bank */
#define REG_FUNC_CFG_ACCESS  0x01   /* bit7: embedded-functions register bank */
#define REG_WHO_AM_I         0x0F
#define REG_CTRL1_XL         0x10
#define REG_CTRL2_G          0x11
#define REG_CTRL3_C          0x12
#define REG_CTRL6_C          0x15

/* Embedded-functions bank */
#define REG_EMB_FUNC_EN_A    0x04   /* bit3: PEDO_EN */
#define REG_EMB_FUNC_EN_B    0x05
#define REG_STEP_COUNTER_L   0x62
#define REG_EMB_FUNC_SRC     0x64   /* bit7: PEDO_RST_STEP */

#define CTRL1_XL_26HZ_2G     0x20
#define CTRL3_C_BDU_IFINC    0x44
#define CTRL3_C_SW_RESET     0x01
#define CTRL6_C_XL_LP        0x10   /* XL_HM_MODE = 1: low-power / normal mode */

static const char *TAG = "lsm6dsox";
static i2c_master_dev_handle_t s_dev;
static uint16_t s_last_raw;
static uint32_t s_total;

static esp_err_t emb_bank(bool on)
{
    return i2c_reg_write8(s_dev, REG_FUNC_CFG_ACCESS, on ? 0x80 : 0x00);
}

esp_err_t lsm6dsox_init(void)
{
    if (!i2c_bus_probe(ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_add(ADDR, &s_dev), TAG, "add");

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(i2c_reg_read(s_dev, REG_WHO_AM_I, &who, 1), TAG, "who_am_i");
    ESP_RETURN_ON_FALSE(who == WHO_AM_I_VAL, ESP_ERR_INVALID_RESPONSE, TAG, "WHO_AM_I 0x%02x", who);

    /* Clean slate: the SPro firmware may have left the IMU configured. */
    ESP_RETURN_ON_ERROR(emb_bank(false), TAG, "bank");
    ESP_RETURN_ON_ERROR(i2c_reg_write8(s_dev, REG_CTRL3_C, CTRL3_C_SW_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(20));   /* >= 10 ms with a 10 ms tick; reset needs ~50 us */
    ESP_RETURN_ON_ERROR(i2c_reg_write8(s_dev, REG_CTRL3_C, CTRL3_C_BDU_IFINC), TAG, "ctrl3");
    ESP_RETURN_ON_ERROR(i2c_reg_write8(s_dev, REG_CTRL2_G, 0x00), TAG, "gyro off");
    ESP_RETURN_ON_ERROR(i2c_reg_write8(s_dev, REG_CTRL6_C, CTRL6_C_XL_LP), TAG, "ctrl6");
    ESP_RETURN_ON_ERROR(i2c_reg_write8(s_dev, REG_CTRL1_XL, CTRL1_XL_26HZ_2G), TAG, "ctrl1");

    /* Pedometer on, everything else in the embedded block off; reset the count. */
    ESP_RETURN_ON_ERROR(emb_bank(true), TAG, "bank");
    esp_err_t err = i2c_reg_write8(s_dev, REG_EMB_FUNC_EN_A, 0x08);
    if (err == ESP_OK) {
        err = i2c_reg_write8(s_dev, REG_EMB_FUNC_EN_B, 0x00);
    }
    if (err == ESP_OK) {
        err = i2c_reg_write8(s_dev, REG_EMB_FUNC_SRC, 0x80);
    }
    emb_bank(false);
    ESP_RETURN_ON_ERROR(err, TAG, "pedometer");

    /* The step-counter reset only lands on a later embedded-function cycle (26 Hz),
     * so wait for it and take whatever the counter reads then as the baseline. */
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t b[2] = { 0 };
    if (emb_bank(true) == ESP_OK) {
        i2c_reg_read(s_dev, REG_STEP_COUNTER_L, b, 2);
        emb_bank(false);
    }
    s_last_raw = (uint16_t)(b[1] << 8 | b[0]);
    s_total = 0;
    ESP_LOGI(TAG, "found, accel 26 Hz low-power, pedometer on");
    return ESP_OK;
}

esp_err_t lsm6dsox_read_steps(uint32_t *steps)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(emb_bank(true), TAG, "bank");
    esp_err_t err = i2c_reg_read(s_dev, REG_STEP_COUNTER_L, b, 2);
    emb_bank(false);
    ESP_RETURN_ON_ERROR(err, TAG, "steps");

    uint16_t raw = (uint16_t)(b[1] << 8 | b[0]);
    if (raw >= s_last_raw) {
        s_total += raw - s_last_raw;
    } else if (s_last_raw > 60000) {
        s_total += (uint32_t)raw + 65536u - s_last_raw;   /* genuine 16-bit wrap */
    } else {
        s_total += raw;                                   /* counter was reset under us */
    }
    s_last_raw = raw;
    *steps = s_total;
    return ESP_OK;
}

void lsm6dsox_power_down(void)
{
    if (!s_dev) {
        return;
    }
    if (emb_bank(true) == ESP_OK) {
        i2c_reg_write8(s_dev, REG_EMB_FUNC_EN_A, 0x00);
        emb_bank(false);
    }
    i2c_reg_write8(s_dev, REG_CTRL1_XL, 0x00);
    i2c_reg_write8(s_dev, REG_CTRL2_G, 0x00);
}
