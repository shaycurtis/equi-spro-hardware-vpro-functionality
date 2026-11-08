#include "i2c_bus.h"

#include "board.h"
#include "esp_check.h"

#define I2C_FREQ_HZ     400000
#define I2C_TIMEOUT_MS  50

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus;

esp_err_t i2c_bus_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* 4.7k on the board */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "bus init");
    return ESP_OK;
}

bool i2c_bus_probe(uint8_t addr)
{
    return s_bus && i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) == ESP_OK;
}

esp_err_t i2c_bus_add(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev, out);
}

esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2c_reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, sizeof(b), I2C_TIMEOUT_MS);
}
