#pragma once
/* Shared I2C master bus on GPIO19 (SCL) / GPIO20 (SDA), 4.7k external pull-ups. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t i2c_bus_init(void);
bool i2c_bus_probe(uint8_t addr);
esp_err_t i2c_bus_add(uint8_t addr, i2c_master_dev_handle_t *out);

/* 8-bit register address helpers. */
esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t i2c_reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
