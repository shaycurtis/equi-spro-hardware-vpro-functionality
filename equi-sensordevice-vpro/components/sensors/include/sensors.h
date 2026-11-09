#pragma once
/*
 * Slow-rate sensors on the I2C bus:
 *   MAX17048  fuel gauge         (0x36)  battery voltage, state of charge, charge rate
 *   SHTC3     temp / humidity    (0x70)  board temperature + RH, sleeps between readings
 *   LSM6DSOX  6-axis IMU         (0x6B)  accelerometer at 26 Hz low-power, hardware pedometer;
 *             + LIS2MDL magnetometer on its sensor hub; streams 9 axes for SD logging
 *   PCF85063A RTC                (0x51)  wall-clock time (coin-cell backed)
 */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    bool     fg_ok;
    uint16_t batt_mv;
    float    batt_soc;        /* % */
    float    batt_rate;       /* %/h, + charging, - discharging */

    bool     th_ok;
    float    temp_c;
    float    rh;

    bool     imu_ok;
    uint32_t steps;           /* since power-on */

    bool     rtc_ok;
    bool     rtc_valid;       /* false if the RTC lost power (oscillator-stop flag) */
    struct tm rtc_time;
} sensor_sample_t;

/* Initialise the bus and every sensor. Missing sensors are logged, not fatal. */
esp_err_t sensors_init(void);

void sensors_read(sensor_sample_t *s);

/* Current RTC time as a UTC Unix timestamp. ESP_ERR_INVALID_STATE if the RTC lost its time. */
esp_err_t sensors_get_epoch(uint32_t *epoch_utc);

/* Set the RTC (and the ESP32 system clock) from a UTC Unix timestamp. */
esp_err_t sensors_set_time(uint32_t epoch_utc);

/*
 * IMU stream (SD logging). Raw int16 axes:
 *   acc  +-16 g      0.488 mg/LSB     at the stream rate (104 or 416 Hz)
 *   gyr  +-2000 dps  70 mdps/LSB      at the stream rate
 *   mag  LIS2MDL     1.5 mgauss/LSB   at 104 Hz (none if the magnetometer is missing)
 * first_us is non-zero in the first batch only: the esp_timer time of its first accel sample.
 */
typedef struct {
    int16_t (*acc)[3];
    int16_t (*gyr)[3];
    int16_t (*mag)[3];
    size_t n_acc, n_gyr, n_mag;
    int64_t first_us;
} imu_batch_t;

typedef void (*imu_stream_cb_t)(const imu_batch_t *b, void *ctx);

/* Start streaming at 104 or 416 Hz; *actual_hz = the sensor's real rate (its oscillator trim).
 * The callback runs in the IMU task. */
esp_err_t sensors_imu_stream_start(uint16_t rate_hz, imu_stream_cb_t cb, void *ctx, float *actual_hz);
void sensors_imu_stream_stop(void);
bool sensors_mag_ok(void);

/* Put sensors into their lowest-power state (power-off / charge-only). */
void sensors_shutdown(void);
