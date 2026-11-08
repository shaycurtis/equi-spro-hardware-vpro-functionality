#include "status_led.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "ws2812.h"

#define LED_DEBUG       0       /* 1: log LED mode changes and frames */
#define FRAME_MS        40      /* 25 fps while breathing */
#define BREATHE_FLOOR   0.06f   /* never fully dark, so it reads as "on" */

typedef struct {
    led_mode_t mode;
    led_colour_t c;
    uint32_t pulse_start;   /* PULSE: when the current flash started (ms) */
} led_state_t;

static const char *TAG = "led";
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
    case LED_MODE_PULSE:
        return (uint32_t)(t_ms - l->pulse_start) < LED_PULSE_ON_MS ? l->c : LED_OFF;
    default:
        return LED_OFF;
    }
}

/* ms until this LED's output next changes (UINT32_MAX = never). */
static uint32_t next_change(const led_state_t *l, uint32_t t_ms)
{
    switch (l->mode) {
    case LED_MODE_BREATHE:
        return FRAME_MS;
    case LED_MODE_BLINK: {
        uint32_t half = LED_BLINK_PERIOD_MS / 2;
        return half - (t_ms % half);
    }
    case LED_MODE_PULSE: {
        uint32_t since = t_ms - l->pulse_start;
        if (since < LED_PULSE_ON_MS) {
            return LED_PULSE_ON_MS - since;
        }
        return since < LED_PULSE_PERIOD_MS ? LED_PULSE_PERIOD_MS - since : 1;
    }
    default:
        return UINT32_MAX;
    }
}

/* Caller holds s_lock. Returns ms until the next render is needed (UINT32_MAX = none). */
static uint32_t render_locked(void)
{
    uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t wait = UINT32_MAX;
    bool changed = !s_shown_valid;
    led_colour_t out[BOARD_LED_COUNT];

    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        if (s_override) {
            out[i] = s_override_c;
        } else {
            if (s_led[i].mode == LED_MODE_PULSE && (uint32_t)(t_ms - s_led[i].pulse_start) >= LED_PULSE_PERIOD_MS) {
                s_led[i].pulse_start = t_ms;   /* due (or overdue): start the next flash now */
            }
            out[i] = compute(&s_led[i], t_ms);
            uint32_t w = next_change(&s_led[i], t_ms);
            wait = w < wait ? w : wait;
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
        esp_err_t err = ws2812_show();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ws2812_show failed: %s", esp_err_to_name(err));
        }
        if (LED_DEBUG) {
            ESP_LOGI(TAG, "show D1=%u,%u,%u D2=%u,%u,%u", out[0].r, out[0].g, out[0].b, out[1].r, out[1].g, out[1].b);
        }
        s_shown_valid = true;
    }
    return wait;
}

static void led_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t wait = render_locked();
        xSemaphoreGive(s_lock);
        /* Sleep until the next LED edge (lets the chip light-sleep between pulses). */
        TickType_t ticks = wait == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait);
        ulTaskNotifyTake(pdTRUE, ticks ? ticks : 1);
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
    if (LED_DEBUG) {
        ESP_LOGI(TAG, "D%d mode %d -> %d (%u,%u,%u)", idx + 1, s_led[idx].mode, mode, c.r, c.g, c.b);
    }
    if (mode == LED_MODE_PULSE && s_led[idx].mode != LED_MODE_PULSE) {
        /* first flash one period from now (immediately after a mode change looks like a glitch) */
        s_led[idx].pulse_start = (uint32_t)(esp_timer_get_time() / 1000) - LED_PULSE_PERIOD_MS + 500;
    }
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
