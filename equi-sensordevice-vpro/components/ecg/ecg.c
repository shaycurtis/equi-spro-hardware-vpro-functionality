#include "ecg.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "board.h"
#include "max30001.h"

/*
 * One task owns the MAX30001:
 *   - heart-rate mode (default): rails on, ECG + hardware R-to-R detector running, the task
 *     polls for new beats every HR_POLL_MS. Light sleep stays allowed (the SPI driver keeps
 *     the clock up only during each transfer).
 *   - capture mode (on request): reconfigure, stream the FIFO to the callback while holding
 *     a no-light-sleep lock, then return to heart-rate mode.
 */

#define RAIL_SETTLE_MS   600    /* 1.8 V LDOs + 32.768 kHz oscillator start-up */
#define POLL_MS          20     /* capture: FIFO is 32 deep = 62 ms at 512 sps */
#define HR_POLL_MS       250    /* heart-rate mode: up to 240 bpm without missing a beat */
#define HR_RETRY_MS      30000  /* AFE not responding: try again later */
#define RR_MIN_MS        300    /* 200 bpm */
#define RR_MAX_MS        3000   /* 20 bpm (resting horses are ~25-45 bpm) */
#define RR_RING          64

static const char *TAG = "ecg";

static esp_pm_lock_handle_t s_pm_lock;
static TaskHandle_t s_task;
static volatile bool s_busy;            /* capture requested or running */
static volatile bool s_stop;            /* stop the running capture early */
static volatile bool s_shutdown;
static volatile bool s_task_done;
static ecg_request_t s_req;

static portMUX_TYPE s_rr_mux = portMUX_INITIALIZER_UNLOCKED;
static struct { uint32_t t_ms, rr_ms; } s_rr[RR_RING];
static uint32_t s_rr_head, s_rr_count;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void awake(bool on)
{
    if (!s_pm_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, TAG, &s_pm_lock) != ESP_OK) {
        return;   /* power management disabled */
    }
    if (on) {
        esp_pm_lock_acquire(s_pm_lock);
    } else {
        esp_pm_lock_release(s_pm_lock);
    }
}

static void rail(bool on)
{
    gpio_set_level(BOARD_PIN_VCC_CTRL, on ? 1 : 0);
}

/* --------------------------------------------------------------- heart rate */

static void record_rr(uint32_t rr)
{
    if (rr < RR_MIN_MS || rr > RR_MAX_MS) {
        return;   /* noise, or a missed beat */
    }
    portENTER_CRITICAL(&s_rr_mux);
    s_rr[s_rr_head] = (typeof(s_rr[0])){ now_ms(), rr };
    s_rr_head = (s_rr_head + 1) % RR_RING;
    if (s_rr_count < RR_RING) {
        s_rr_count++;
    }
    portEXIT_CRITICAL(&s_rr_mux);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void ecg_hr_get(uint32_t window_s, uint8_t *bpm, uint8_t *beats)
{
    uint32_t v[RR_RING], n = 0, since = now_ms() - window_s * 1000;
    portENTER_CRITICAL(&s_rr_mux);
    for (uint32_t i = 0; i < s_rr_count; i++) {
        uint32_t k = (s_rr_head + RR_RING - 1 - i) % RR_RING;
        if ((int32_t)(s_rr[k].t_ms - since) >= 0) {
            v[n++] = s_rr[k].rr_ms;
        }
    }
    portEXIT_CRITICAL(&s_rr_mux);
    *beats = (uint8_t)(n > 255 ? 255 : n);
    if (n < 3) {
        *bpm = 0;   /* not enough beats: electrodes off, or not detecting */
        return;
    }
    qsort(v, n, sizeof(v[0]), cmp_u32);
    uint32_t med = (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    uint32_t b = (60000 + med / 2) / med;
    *bpm = (uint8_t)(b > 255 ? 255 : b);
}

/* Configure heart-rate mode. Powers the rails if needed. */
static esp_err_t hr_setup(bool rails_were_on)
{
    if (!rails_were_on) {
        rail(true);
        vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
    }
    uint32_t info = 0;
    esp_err_t err = max30001_bus_init();
    if (err == ESP_OK) {
        err = max30001_probe(&info);
    }
    if (err == ESP_OK) {
        err = max30001_ecg_start(false);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "heart-rate monitoring on (R-to-R detector, poll %d ms)", HR_POLL_MS);
    }
    return err;
}

/* ------------------------------------------------------------------ capture */

static void run_capture(void)
{
    const uint32_t target = (uint32_t)s_req.seconds * ECG_RATE_HZ;
    int16_t block[ECG_BLOCK_SAMPLES];
    size_t nblock = 0;
    uint32_t total = 0, overflows = 0, block_start = 0;
    int32_t raw[32];

    awake(true);
    esp_err_t status = max30001_ecg_start(s_req.test_signal);   /* rails already on */

    int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    while (status == ESP_OK && !s_stop && !s_shutdown && total < target) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));
        bool ovf;
        size_t n = max30001_ecg_read(raw, sizeof(raw) / sizeof(raw[0]), &ovf);
        if (ovf) {
            overflows++;
        }
        for (size_t i = 0; i < n && total < target; i++) {
            int32_t v = raw[i] >> 2;    /* 18 -> 16 bit */
            block[nblock++] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
            total++;
            if (nblock == ECG_BLOCK_SAMPLES) {
                s_req.on_block(block, nblock, block_start, s_req.ctx);
                block_start += nblock;
                nblock = 0;
            }
        }
        uint32_t rr;
        if (max30001_rtor_poll(&rr)) {
            record_rr(rr);          /* keep heart rate going during a capture too */
        }
    }
    if (nblock) {
        s_req.on_block(block, nblock, block_start, s_req.ctx);
    }
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    awake(false);
    ESP_LOGI(TAG, "capture done: %lu samples in %lld ms (%.1f sps), %lu FIFO overflows, status %s",
             (unsigned long)total, dt_ms, dt_ms ? total * 1000.0 / dt_ms : 0.0,
             (unsigned long)overflows, esp_err_to_name(status));

    s_busy = false;
    if (s_req.on_done) {
        s_req.on_done(status, total, overflows, s_req.ctx);
    }
}

/* --------------------------------------------------------------------- task */

static void ecg_task(void *arg)
{
    bool rails_on = false, hr_ok = false;
    uint32_t hr_retry_at = 0;

    while (!s_shutdown) {
        if (s_busy) {                       /* capture requested */
            if (!rails_on) {
                rail(true);
                vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
                rails_on = true;
                max30001_bus_init();
                uint32_t info;
                max30001_probe(&info);
            }
            run_capture();
            hr_ok = false;                  /* reconfigure heart-rate mode afterwards */
            continue;
        }
        if (!hr_ok && (int32_t)(now_ms() - hr_retry_at) >= 0) {
            hr_ok = hr_setup(rails_on) == ESP_OK;
            rails_on = true;
            if (!hr_ok) {
                ESP_LOGW(TAG, "MAX30001 not responding; heart rate unavailable, retrying in %d s",
                         HR_RETRY_MS / 1000);
                hr_retry_at = now_ms() + HR_RETRY_MS;
            }
        }
        if (hr_ok) {
            uint32_t rr;
            if (max30001_rtor_poll(&rr)) {
                record_rr(rr);
            }
        }
        /* Wakes early when a capture is requested or on shutdown. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HR_POLL_MS));
    }

    max30001_stop();
    rail(false);
    s_task_done = true;
    vTaskDelete(NULL);
}

esp_err_t ecg_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    return xTaskCreate(ecg_task, "ecg", 4096, NULL, 6, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ecg_start(const ecg_request_t *req)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!req->on_block || req->seconds == 0 || req->seconds > ECG_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_req = *req;
    s_stop = false;
    s_busy = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void ecg_stop(void)
{
    s_stop = true;
}

bool ecg_busy(void)
{
    return s_busy;
}

void ecg_shutdown(void)
{
    s_shutdown = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
        for (int i = 0; i < 40 && !s_task_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    rail(false);
}
