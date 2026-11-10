#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "sensors.h"

/* MAX17048 */
esp_err_t max17048_init(void);
esp_err_t max17048_read(uint16_t *mv, float *soc, float *rate);

/* SHTC3 */
esp_err_t shtc3_init(void);
esp_err_t shtc3_read(float *temp_c, float *rh);

/* LSM6DSOX */
esp_err_t lsm6dsox_init(void);
esp_err_t lsm6dsox_read_steps(uint32_t *steps);
void lsm6dsox_power_down(void);
esp_err_t lsm6dsox_stream_start(uint16_t rate_hz, imu_stream_cb_t cb, void *ctx, float *actual_hz);
void lsm6dsox_stream_stop(void);
bool lsm6dsox_mag_ok(void);
void lsm6dsox_posture_poll(void);   /* drain the idle FIFO into the posture detector */

/* Posture (posture.c). Feeds are wrapped in posture_begin() / posture_end(). */
typedef struct {
    uint8_t position, seen;
    float tilt_deg, pitch_deg, grav[3];
} posture_state_t;
void posture_reset(void);
void posture_begin(void);
void posture_end(void);
/* One accelerometer sample; `window` samples make one ~1 s window, `one_g` = LSB per g. */
void posture_feed_acc(const int16_t xyz[3], int window, int32_t one_g);
void posture_feed_step(void);       /* the pedometer detected a step */
void posture_get(posture_state_t *out);
void posture_clear_seen(void);

/* PCF85063A */
esp_err_t pcf85063_init(void);
esp_err_t pcf85063_read(struct tm *t, bool *valid);
esp_err_t pcf85063_write(const struct tm *t);
