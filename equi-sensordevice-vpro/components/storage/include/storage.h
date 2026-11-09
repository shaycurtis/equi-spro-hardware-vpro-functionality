#pragma once
/*
 * microSD storage (SPI mode on SPI3, FAT at STORAGE_MOUNT) and the device configuration file.
 *
 * /sdcard/config.json (created with defaults if missing; the web app will edit it later):
 *   {
 *     "version": 1,
 *     "ecg": { "log": true,  "rate_hz": 512 },     rate_hz: 256 or 512
 *     "imu": { "log": false, "rate_hz": 104 }      rate_hz: 104 or 416 (accel + gyro);
 *   }                                              the magnetometer is logged at 104 Hz
 * Each session records the enabled channels automatically.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_MOUNT  "/sdcard"

typedef struct {
    bool ecg_log;
    uint16_t ecg_rate_hz;
    bool imu_log;
    uint16_t imu_rate_hz;
} storage_config_t;

/* Mount the card and load the configuration (defaults if there is no card or no file). */
esp_err_t storage_init(void);

bool storage_mounted(void);

/* The configuration loaded at boot. */
const storage_config_t *storage_config(void);

/* Bring-up test: write, read back and delete a 1 MB file, logging the speeds. */
esp_err_t storage_speed_test(void);

/* Shutdown hook: unmount so every file is flushed and closed. */
void storage_shutdown(void);
