#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

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

/* PCF85063A */
esp_err_t pcf85063_init(void);
esp_err_t pcf85063_read(struct tm *t, bool *valid);
