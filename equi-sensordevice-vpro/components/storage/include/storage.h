#pragma once
/*
 * microSD storage (SPI mode on SPI3, FAT at STORAGE_MOUNT) and the device configuration file.
 *
 * /sdcard/config.json (created with defaults if missing; the web app will edit it later):
 *   {
 *     "version": 1,
 *     "ecg": { "log": false, "rate_hz": 256 },     rate_hz: 256 or 512
 *     "imu": { "log": false, "rate_hz": 104 },     rate_hz: 104 or 416 (accel + gyro);
 *                                                  the magnetometer is logged at 104 Hz
 *     "gnss": { "log": false },                    stored; GNSS logging does not exist yet
 *     "debug": { "live_log": false }               stored; for the live log (later phase)
 *   }
 * Each session records the enabled channels automatically. The file is read at start-up; it can
 * be changed remotely through the AWS IoT Device Shadow while charging (components/iot,
 * storage_config_update), which takes effect at the next power-on.
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
    bool gnss_log;       /* accepted and stored; no effect yet */
    bool live_log;       /* accepted and stored; no effect yet */
} storage_config_t;

struct cJSON;

/* Mount the card and load the configuration (defaults if there is no card or no file). */
esp_err_t storage_init(void);

bool storage_mounted(void);

/* The configuration loaded at boot. */
const storage_config_t *storage_config(void);

/* Add the settings to a JSON object, in config.json's layout (ecg, imu, gnss, debug). */
void storage_config_to_json(struct cJSON *obj);

/*
 * Apply settings from a JSON object in config.json's layout (e.g. the shadow's "desired"). Only
 * known fields with valid values are taken (others are logged and ignored); missing fields keep
 * their value. A change is written to config.json. Returns true if anything changed. Does
 * nothing (false) without a mounted card: the settings could not be kept.
 */
bool storage_config_update(const struct cJSON *obj);

/* Bring-up test: write, read back and delete a 1 MB file, logging the speeds. */
esp_err_t storage_speed_test(void);

/* Shutdown hook: unmount so every file is flushed and closed. */
void storage_shutdown(void);
