#pragma once
/*
 * Status LEDs (WS2812 chain, D1 = index 0, D2 = index 1). Thread-safe.
 *
 * Two layers:
 *   - per-LED mode (off / solid / breathe), the normal "what is the device doing" state
 *   - an override colour shown on BOTH LEDs, for short feedback (power on/off, holds)
 *
 * The WS2812s latch their colour, so a solid colour set before deep sleep stays lit
 * while the 3.3 V rail is up (i.e. while USB is connected).
 */
#include <stdint.h>
#include "esp_err.h"

typedef struct { uint8_t r, g, b; } led_colour_t;

#define LED_OFF     ((led_colour_t){ 0, 0, 0 })
#define LED_RED     ((led_colour_t){ 24, 0, 0 })
#define LED_GREEN   ((led_colour_t){ 0, 24, 0 })
#define LED_BLUE    ((led_colour_t){ 0, 0, 24 })
#define LED_ORANGE  ((led_colour_t){ 24, 8, 0 })
#define LED_WHITE   ((led_colour_t){ 10, 10, 10 })
#define LED_RUNNING ((led_colour_t){ 0, 10, 40 })   /* "device on" colour (pulse / breathe peak) */
#define LED_LINKED  ((led_colour_t){ 0, 32, 4 })    /* heartbeat colour while a gateway hears us */

#define LED_IDX_STATUS  0   /* D1: device on (heartbeat pulse) */
#define LED_IDX_CHARGE  1   /* D2: charge state */

#define LED_BREATHE_PERIOD_MS  4000
#define LED_BLINK_PERIOD_MS    500
#define LED_PULSE_PERIOD_MS    5000
#define LED_PULSE_ON_MS        60

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_SOLID,
    LED_MODE_BREATHE,
    LED_MODE_BLINK,         /* 2 Hz on/off, phase-locked across LEDs so they flash together */
    LED_MODE_PULSE,         /* short flash every LED_PULSE_PERIOD_MS ("heartbeat"), very low power */
} led_mode_t;

esp_err_t status_led_init(void);

/* Normal per-LED state. */
void status_led_set_mode(int idx, led_mode_t mode, led_colour_t c);

/* Override: both LEDs show `c` until status_led_override_clear(). */
void status_led_set(led_colour_t c);
void status_led_override_clear(void);

/* Blocking: flash both LEDs `count` times, then return to the per-LED modes. */
void status_led_flash(led_colour_t c, int count, int on_ms, int off_ms);
