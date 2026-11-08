#pragma once
/*
 * Slow-rate sensors on the I2C bus:
 *   MAX17048  fuel gauge         (0x36)  battery voltage, state of charge, charge rate
 *   SHTC3     temp / humidity    (0x70)  board temperature + RH, sleeps between readings
 *   LSM6DSOX  6-axis IMU         (0x6B)  accelerometer at 26 Hz low-power, hardware pedometer
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

/* Put sensors into their lowest-power state (power-off / charge-only). */
void sensors_shutdown(void);
