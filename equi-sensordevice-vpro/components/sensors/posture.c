/*
 * Horse position (posture) from the accelerometer's gravity direction.
 *
 * The same classes and numbering as the VPro platform (its LoRa parser's position_list):
 * unknown, lateral, sternal, rolling, standing, moving. The method is the threshold one of
 * VPro's "PositionDetection_simple" (VPro's current firmware uses a trained model instead):
 *
 *   1. Average the accelerometer over ~1 s windows: the mean is gravity.
 *   2. Tilt = the left / right roll angle: gravity's angle from the "standing" axis
 *      (POSTURE_UP_*), measured about the horse's head-to-tail axis (POSTURE_FWD_*):
 *        < 25 deg standing, 25-60 sternal, 60-120 lateral, > 120 (belly up) rolling,
 *      with +-5 deg of hysteresis. Tipped more than 60 deg forward or back (pitch) the roll
 *      angle is not meaningful and the last class is kept. Two lateral windows on opposite sides within 5 s also count
 *      as rolling (a roll over the back that the 1 s average did not catch belly-up).
 *   3. Upright with a pedometer step in the window = moving.
 *   4. A new class has to last its debounce time (VPro's values) before it becomes the
 *      position.
 *
 * The thresholds are starting values: they need tuning from a recording on a horse.
 * Callers hold posture_begin() / posture_end() around a batch of feeds.
 */
#include "sensors_priv.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Sensor axis that points up (reads +1 g) when the horse stands: +X with the board vertical on
 * the chest as worn (measured 2026-11-10 on 236B2C, held by hand: 0.99 -0.05 0.11 g). */
#define POSTURE_UP_X        1.0f
#define POSTURE_UP_Y        0.0f
#define POSTURE_UP_Z        0.0f
/* Sensor axis along the horse, head to tail (either sign): the board's normal, Z, with the
 * board flat against the chest. The horse rolls about this axis when it lies down, so only the
 * tilt across it (left / right, sensor Y) is classified; tipping along it (pitch) is not. */
#define POSTURE_FWD_X       0.0f
#define POSTURE_FWD_Y       0.0f
#define POSTURE_FWD_Z       1.0f
#define PITCH_MAX_DEG       60.0f    /* tipped further forward / back than this: not classified */

#define TILT_STERNAL_DEG    25.0f
#define TILT_LATERAL_DEG    60.0f
#define TILT_ROLLING_DEG    120.0f
#define TILT_HYST_DEG       5.0f
#define ROLL_OVER_WINDOWS   5        /* side-to-side within this many windows = rolling */
#define FIRST_FIX_WINDOWS   2        /* debounce of the very first position */
#define G_MIN               0.7f     /* windows whose mean is not ~1 g are not classified */
#define G_MAX               1.3f

/* Debounce per position (windows ~ seconds), VPro's values: lateral 3.3, sternal 39,
 * rolling 1.5, upright 7.7; moving is immediate. */
static const uint8_t DEBOUNCE[POSTURE_COUNT] = {
    [POSTURE_UNKNOWN] = 1, [POSTURE_LATERAL] = 3, [POSTURE_STERNAL] = 39,
    [POSTURE_ROLLING] = 2, [POSTURE_STANDING] = 8, [POSTURE_MOVING] = 1,
};

enum { TILT_UPRIGHT, TILT_LEANING, TILT_SIDEWAYS, TILT_INVERTED };
static const float TILT_LIMIT[3] = { TILT_STERNAL_DEG, TILT_LATERAL_DEG, TILT_ROLLING_DEG };

static SemaphoreHandle_t s_mx;
static int64_t s_sum[3];
static int s_n;
static bool s_step;                       /* a step in the current window */

static int s_tilt_cls = TILT_UPRIGHT;
static float s_side[ROLL_OVER_WINDOWS][3];   /* unit gravity of recent windows */
static bool s_side_lateral[ROLL_OVER_WINDOWS];
static int s_side_idx;

static uint8_t s_raw, s_cand, s_cur;      /* POSTURE_* */
static uint16_t s_cand_windows;
static uint8_t s_seen;                    /* bit n = position n held since the last clear */
static float s_tilt_deg = -1, s_pitch_deg, s_grav[3];

void posture_begin(void)
{
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
}

void posture_end(void)
{
    xSemaphoreGive(s_mx);
}

void posture_reset(void)
{
    posture_begin();
    memset(s_sum, 0, sizeof(s_sum));
    memset(s_side_lateral, 0, sizeof(s_side_lateral));
    s_n = 0;
    s_step = false;
    s_tilt_cls = TILT_UPRIGHT;
    s_raw = s_cand = s_cur = POSTURE_UNKNOWN;
    s_cand_windows = 0;
    s_seen = 0;
    s_tilt_deg = -1;
    posture_end();
}

/* Tilt class with hysteresis: a limit moves away from the class we are in. */
static int tilt_class(float tilt)
{
    int cls = TILT_UPRIGHT;
    for (int i = 0; i < 3; i++) {
        float limit = TILT_LIMIT[i] + (s_tilt_cls <= i ? TILT_HYST_DEG : -TILT_HYST_DEG);
        if (tilt >= limit) {
            cls = i + 1;
        }
    }
    return cls;
}

static void window_done(int32_t one_g)
{
    float g[3], mag2 = 0;
    for (int i = 0; i < 3; i++) {
        g[i] = (float)s_sum[i] / ((float)s_n * (float)one_g);
        mag2 += g[i] * g[i];
        s_sum[i] = 0;
    }
    s_n = 0;
    bool step = s_step;
    s_step = false;

    float mag = sqrtf(mag2);
    float u[3] = { 0 }, fwd = 0, side = 0;
    bool usable = mag >= G_MIN && mag <= G_MAX;
    if (usable) {
        static const float UP[3] = { POSTURE_UP_X, POSTURE_UP_Y, POSTURE_UP_Z };
        static const float FWD[3] = { POSTURE_FWD_X, POSTURE_FWD_Y, POSTURE_FWD_Z };
        float up = 0;
        for (int i = 0; i < 3; i++) {
            u[i] = g[i] / mag;
            up += u[i] * UP[i];
            fwd += u[i] * FWD[i];
        }
        /* What is left after the up and forward parts is the horse's left / right axis. */
        float s2 = 1.0f - up * up - fwd * fwd;
        side = s2 > 0 ? sqrtf(s2) : 0;
        fwd = fwd > 1 ? 1 : fwd < -1 ? -1 : fwd;
        s_pitch_deg = asinf(fwd) * 57.29578f;
        s_tilt_deg = atan2f(side, up) * 57.29578f;     /* roll, 0 (upright) .. 180 (belly up) */
        memcpy(s_grav, g, sizeof(s_grav));
        /* Tipped mostly forward or back: the roll angle means little, keep the last class. */
        usable = fabsf(s_pitch_deg) < PITCH_MAX_DEG;
    }
    if (usable) {
        s_tilt_cls = tilt_class(s_tilt_deg);

        bool rolled = false;
        if (s_tilt_cls == TILT_SIDEWAYS) {
            for (int i = 0; i < ROLL_OVER_WINDOWS; i++) {
                if (s_side_lateral[i] &&
                    u[0] * s_side[i][0] + u[1] * s_side[i][1] + u[2] * s_side[i][2] < 0) {
                    rolled = true;    /* on the other side a moment ago */
                }
            }
        }
        memcpy(s_side[s_side_idx], u, sizeof(u));
        s_side_lateral[s_side_idx] = s_tilt_cls == TILT_SIDEWAYS;
        s_side_idx = (s_side_idx + 1) % ROLL_OVER_WINDOWS;

        switch (s_tilt_cls) {
        case TILT_UPRIGHT:  s_raw = step ? POSTURE_MOVING : POSTURE_STANDING; break;
        case TILT_LEANING:  s_raw = POSTURE_STERNAL; break;
        case TILT_SIDEWAYS: s_raw = rolled ? POSTURE_ROLLING : POSTURE_LATERAL; break;
        default:            s_raw = POSTURE_ROLLING; break;
        }
    }
    /* else: a shock, free fall or tipped forward / back: keep the last class */

    if (s_raw != s_cand) {
        s_cand = s_raw;
        s_cand_windows = 0;
    }
    if (s_cand_windows < UINT16_MAX) {
        s_cand_windows++;
    }
    if (s_cand != s_cur &&
        s_cand_windows >= (s_cur == POSTURE_UNKNOWN ? FIRST_FIX_WINDOWS : DEBOUNCE[s_cand])) {
        s_cur = s_cand;
    }
    s_seen |= (uint8_t)(1u << s_cur);
}

void posture_feed_acc(const int16_t xyz[3], int window, int32_t one_g)
{
    s_sum[0] += xyz[0];
    s_sum[1] += xyz[1];
    s_sum[2] += xyz[2];
    if (++s_n >= window) {
        window_done(one_g);
    }
}

void posture_feed_step(void)
{
    s_step = true;
}

void posture_get(posture_state_t *out)
{
    posture_begin();
    out->position = s_cur;
    out->seen = s_seen & (uint8_t)~(1u << POSTURE_UNKNOWN);
    out->tilt_deg = s_tilt_deg;
    out->pitch_deg = s_pitch_deg;
    memcpy(out->grav, s_grav, sizeof(s_grav));
    posture_end();
}

void posture_clear_seen(void)
{
    posture_begin();
    s_seen = 0;
    posture_end();
}
