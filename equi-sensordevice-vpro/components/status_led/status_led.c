#include "status_led.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "board.h"
#include "ws2812.h"

#define FRAME_MS        40      /* 25 fps while breathing */
#define BREATHE_FLOOR   0.06f   /* never fully dark, so it reads as "on" */

typedef struct {
    led_mode_t mode;
    led_colour_t c;
} led_state_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static led_state_t s_led[BOARD_LED_COUNT];
static bool s_override;
static led_colour_t s_override_c;
static led_colour_t s_shown[BOARD_LED_COUNT];
static bool s_shown_valid;

static uint8_t scale(uint8_t v, float k)
{
    return (uint8_t)(v * k + 0.5f);
}

static led_colour_t compute(const led_state_t *l, uint32_t t_ms)
{
    switch (l->mode) {
    case LED_MODE_SOLID:
        return l->c;
    case LED_MODE_BREATHE: {
        float phase = (float)(t_ms % LED_BREATHE_PERIOD_MS) / LED_BREATHE_PERIOD_MS;
        float k = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
        k = BREATHE_FLOOR + (1.0f - BREATHE_FLOOR) * k * k;   /* rough gamma */
        return (led_colour_t){ scale(l->c.r, k), scale(l->c.g, k), scale(l->c.b, k) };
    }
    case LED_MODE_BLINK:
        return (t_ms % LED_BLINK_PERIOD_MS) < LED_BLINK_PERIOD_MS / 2 ? l->c : LED_OFF;
    default:
        return LED_OFF;
    }
}

/* Caller holds s_lock. Returns true if any LED is animating. */
static bool render_locked(void)
{
    uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool animating = false;
    bool changed = !s_shown_valid;
    led_colour_t out[BOARD_LED_COUNT];

    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        if (s_override) {
            out[i] = s_override_c;
        } else {
            out[i] = compute(&s_led[i], t_ms);
            animating |= (s_led[i].mode == LED_MODE_BREATHE || s_led[i].mode == LED_MODE_BLINK);
        }
        if (memcmp(&out[i], &s_shown[i], sizeof(out[i])) != 0) {
            changed = true;
        }
    }
    if (changed) {
        for (int i = 0; i < BOARD_LED_COUNT; i++) {
            ws2812_set(i, out[i].r, out[i].g, out[i].b);
            s_shown[i] = out[i];
        }
        ws2812_show();
        s_shown_valid = true;
    }
    return animating;
}

static void led_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool animating = render_locked();
        xSemaphoreGive(s_lock);
        /* Only tick while something is animating; otherwise sleep until a change. */
        ulTaskNotifyTake(pdTRUE, animating ? pdMS_TO_TICKS(FRAME_MS) : portMAX_DELAY);
    }
}

/* Apply a change now (so it is visible even right before deep sleep) and wake the task. */
static void update(void)
{
    render_locked();
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

esp_err_t status_led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ws2812_init(BOARD_PIN_LED, BOARD_LED_COUNT);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(led_task, "led", 2560, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void status_led_set_mode(int idx, led_mode_t mode, led_colour_t c)
{
    if (idx < 0 || idx >= BOARD_LED_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_led[idx].mode = mode;
    s_led[idx].c = c;
    update();
}

void status_led_set(led_colour_t c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = true;
    s_override_c = c;
    update();
}

void status_led_override_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = false;
    update();
}

void status_led_flash(led_colour_t c, int count, int on_ms, int off_ms)
{
    for (int i = 0; i < count; i++) {
        status_led_set(c);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        status_led_set(LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
    status_led_override_clear();
}
