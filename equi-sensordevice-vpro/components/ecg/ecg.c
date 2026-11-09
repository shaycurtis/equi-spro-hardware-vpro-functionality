#include "ecg.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
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
 *   - stream mode (ecg_stream_start, e.g. SD recording): continuous samples at 512 or 256 sps.
 *     INTB goes low once the FIFO holds STREAM_IRQ_SAMPLES; the task wakes on it, drains the
 *     FIFO and goes back to sleep, so light sleep continues between reads. A timed poll
 *     (STREAM_POLL_MS) is the fallback if the interrupt never comes. Heart rate keeps running,
 *     and 512 sps captures are served from the same stream.
 *   - capture mode (on request, no stream): reconfigure, stream the FIFO to the callback while
 *     holding a no-light-sleep lock, then return to heart-rate mode.
 */

#define RAIL_SETTLE_MS   600    /* 1.8 V LDOs + 32.768 kHz oscillator start-up */
#define POLL_MS          20     /* capture: FIFO is 32 deep = 62 ms at 512 sps */
#define HR_POLL_MS       250    /* heart-rate mode: up to 240 bpm without missing a beat */
#define HR_RETRY_MS      30000  /* AFE not responding: try again later */
#define RR_MIN_MS        300    /* 200 bpm */
#define RR_MAX_MS        3000   /* 20 bpm (resting horses are ~25-45 bpm) */
#define RR_RING          64
#define STREAM_IRQ_SAMPLES 16   /* 31 ms at 512 sps, half the 32-sample FIFO */
#define STREAM_POLL_MS   40     /* fallback poll: 20 samples at 512 sps */
#define SPURIOUS_LIMIT   50     /* INTB low with an empty FIFO this often in a row: stop using it */
#define STATS_PERIOD_MS  60000

static const char *TAG = "ecg";

static esp_pm_lock_handle_t s_pm_lock;
static TaskHandle_t s_task;
static volatile bool s_busy;            /* capture requested or running */
static volatile bool s_stop;            /* stop the running capture early */
static volatile bool s_shutdown;
static volatile bool s_task_done;
static ecg_request_t s_req;

/* stream (continuous) mode */
static volatile uint16_t s_stream_rate;          /* running rate, 0 = off (SD stream or live tap) */
static volatile uint16_t s_rec_rate;             /* rate the SD stream asked for, 0 = none */
static ecg_stream_cb_t volatile s_stream_cb;
static void *s_stream_ctx;
static ecg_live_cb_t volatile s_live_cb;
static void *s_live_ctx;
static uint32_t s_live_phase;                    /* decimation phase at 512 sps */
static volatile bool s_stream_changed;
static bool s_irq_ok = true;                     /* INTB usable */
static bool s_isr_added;

/* capture bookkeeping (shared by the polled capture and the stream tap) */
static struct {
    bool active;
    uint32_t target, total, overflows, block_start;
    size_t nblock;
    int16_t block[ECG_BLOCK_SAMPLES];
    int64_t t0;
} s_cap;

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

static inline int16_t to16(int32_t raw)
{
    int32_t v = raw >> 2;    /* 18 -> 16 bit */
    return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
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

static uint32_t poll_rr(void)
{
    uint32_t rr;
    if (max30001_rtor_poll(&rr)) {
        record_rr(rr);
        return rr;
    }
    return 0;
}

/* ------------------------------------------------------------------ capture */

static void capture_begin(void)
{
    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.active = true;
    s_cap.target = (uint32_t)s_req.seconds * ECG_RATE_HZ;
    s_cap.t0 = esp_timer_get_time();
}

/* Feed 512 sps samples to the running capture; true once it has all it asked for. */
static bool capture_feed(const int16_t *v, size_t n)
{
    for (size_t i = 0; i < n && s_cap.total < s_cap.target; i++) {
        s_cap.block[s_cap.nblock++] = v[i];
        s_cap.total++;
        if (s_cap.nblock == ECG_BLOCK_SAMPLES) {
            s_req.on_block(s_cap.block, s_cap.nblock, s_cap.block_start, s_req.ctx);
            s_cap.block_start += s_cap.nblock;
            s_cap.nblock = 0;
        }
    }
    return s_cap.total >= s_cap.target;
}

static void capture_finish(esp_err_t status)
{
    if (s_cap.nblock) {
        s_req.on_block(s_cap.block, s_cap.nblock, s_cap.block_start, s_req.ctx);
    }
    int64_t dt_ms = (esp_timer_get_time() - s_cap.t0) / 1000;
    ESP_LOGI(TAG, "capture done: %lu samples in %lld ms (%.1f sps), %lu FIFO overflows, status %s",
             (unsigned long)s_cap.total, dt_ms, dt_ms ? s_cap.total * 1000.0 / dt_ms : 0.0,
             (unsigned long)s_cap.overflows, esp_err_to_name(status));
    s_cap.active = false;
    s_busy = false;
    if (s_req.on_done) {
        s_req.on_done(status, s_cap.total, s_cap.overflows, s_req.ctx);
    }
}

/* Capture without a stream: reconfigure (test signal possible), poll, then back to HR mode. */
static void run_capture(void)
{
    int32_t raw[32];
    int16_t v[32];

    awake(true);
    esp_err_t status = max30001_ecg_start(s_req.test_signal, ECG_RATE_HZ, 0);   /* rails already on */
    capture_begin();
    bool done = false;
    TickType_t last = xTaskGetTickCount();
    while (status == ESP_OK && !s_stop && !s_shutdown && !done) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));
        bool ovf;
        size_t n = max30001_ecg_read(raw, sizeof(raw) / sizeof(raw[0]), &ovf);
        if (ovf) {
            s_cap.overflows++;
        }
        for (size_t i = 0; i < n; i++) {
            v[i] = to16(raw[i]);
        }
        done = capture_feed(v, n);
        poll_rr();                  /* keep heart rate going during a capture too */
    }
    awake(false);
    capture_finish(status);
}

/* -------------------------------------------------------------------- stream */

static void IRAM_ATTR intb_isr(void *arg)
{
    gpio_intr_disable(BOARD_PIN_ECG_INT);   /* level interrupt: re-enabled after the FIFO is read */
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void intb_setup(bool on)
{
    if (on) {
        gpio_config_t io = {
            .pin_bit_mask = BIT64(BOARD_PIN_ECG_INT),
            .mode = GPIO_MODE_INPUT,
            .intr_type = GPIO_INTR_LOW_LEVEL,   /* MAX30001 pull-up holds it at OVDD (1.8 V) */
        };
        gpio_config(&io);
        gpio_sleep_sel_dis(BOARD_PIN_ECG_INT);  /* stay an input through light sleep */
        if (!s_isr_added && gpio_isr_handler_add(BOARD_PIN_ECG_INT, intb_isr, NULL) == ESP_OK) {
            s_isr_added = true;
        }
        gpio_wakeup_enable(BOARD_PIN_ECG_INT, GPIO_INTR_LOW_LEVEL);   /* wakes the chip from light sleep */
        gpio_intr_enable(BOARD_PIN_ECG_INT);
    } else {
        gpio_intr_disable(BOARD_PIN_ECG_INT);
        gpio_wakeup_disable(BOARD_PIN_ECG_INT);
    }
}

static esp_err_t stream_setup(void)
{
    esp_err_t err = max30001_ecg_start(false, s_stream_rate, s_irq_ok ? STREAM_IRQ_SAMPLES : 0);
    if (err == ESP_OK) {
        intb_setup(s_irq_ok);
        ESP_LOGI(TAG, "streaming %u sps (%s), heart rate on", s_stream_rate,
                 s_irq_ok ? "FIFO interrupt" : "polled");
    }
    return err;
}

static struct {
    uint32_t wakes, samples, overflows, spurious_run;
    uint32_t since_ms;
} s_st;

/* Drain the FIFO into the stream callback (and a 512 sps capture, if one is running). */
static void stream_service(void)
{
    int32_t raw[32];
    int16_t v[32];
    int32_t live[32];
    size_t got = 0;
    s_st.wakes++;
    uint32_t rr = poll_rr();
    for (int pass = 0; pass < 3; pass++) {
        bool ovf;
        size_t n = max30001_ecg_read(raw, 32, &ovf);
        if (ovf) {
            s_st.overflows++;
            if (s_cap.active) {
                s_cap.overflows++;
            }
        }
        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            v[i] = to16(raw[i]);
        }
        got += n;
        ecg_stream_cb_t cb = s_stream_cb;
        if (cb) {
            cb(v, n, s_stream_ctx);
        }
        ecg_live_cb_t lcb = s_live_cb;
        if (lcb) {
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (s_stream_rate == ECG_LIVE_RATE_HZ || (s_live_phase++ & 1) == 0) {
                    live[m++] = raw[i];
                }
            }
            if (m) {
                lcb(live, m, (uint16_t)(rr > 0xffff ? 0 : rr), s_live_ctx);
                rr = 0;
            }
        }
        if (s_busy && !s_cap.active) {
            capture_begin();
        }
        if (s_cap.active && (capture_feed(v, n) || s_stop)) {
            capture_finish(ESP_OK);
        }
        if (n < 32) {
            break;
        }
    }
    s_st.samples += got;

    /* INTB stuck low (e.g. its 1.8 V high level not read as high): fall back to polling. */
    if (s_irq_ok) {
        if (got == 0 && gpio_get_level(BOARD_PIN_ECG_INT) == 0) {
            if (++s_st.spurious_run >= SPURIOUS_LIMIT) {
                ESP_LOGW(TAG, "INTB stays low with an empty FIFO: switching to polling");
                s_irq_ok = false;
                intb_setup(false);
            }
        } else {
            s_st.spurious_run = 0;
        }
    }
    if (now_ms() - s_st.since_ms >= STATS_PERIOD_MS) {
        ESP_LOGI(TAG, "stream: %lu samples, %lu reads, %lu overflows in the last %lu s",
                 (unsigned long)s_st.samples, (unsigned long)s_st.wakes, (unsigned long)s_st.overflows,
                 (unsigned long)((now_ms() - s_st.since_ms) / 1000));
        s_st.samples = s_st.wakes = s_st.overflows = 0;
        s_st.since_ms = now_ms();
    }
}

/* --------------------------------------------------------------------- task */

/* Bring the AFE up in heart-rate or stream mode. */
static esp_err_t mode_setup(bool *rails_on)
{
    if (!*rails_on) {
        rail(true);
        vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
        *rails_on = true;
    }
    uint32_t info = 0;
    esp_err_t err = max30001_bus_init();
    if (err == ESP_OK) {
        err = max30001_probe(&info);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (s_stream_rate) {
        s_st.since_ms = now_ms();
        return stream_setup();
    }
    intb_setup(false);
    err = max30001_ecg_start(false, ECG_RATE_HZ, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "heart-rate monitoring on (R-to-R detector, poll %d ms)", HR_POLL_MS);
    }
    return err;
}

static void ecg_task(void *arg)
{
    bool rails_on = false, ok = false;
    uint32_t retry_at = 0;

    while (!s_shutdown) {
        if (s_stream_changed) {
            s_stream_changed = false;
            if (s_cap.active) {
                capture_finish(ESP_ERR_INVALID_STATE);   /* its source went away */
            }
            ok = false;                     /* reconfigure for the new mode */
            retry_at = now_ms();
        }
        if (s_busy && !s_stream_rate) {     /* capture requested, no stream to serve it */
            if (!rails_on) {
                rail(true);
                vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
                rails_on = true;
                max30001_bus_init();
                uint32_t info;
                max30001_probe(&info);
            }
            run_capture();
            ok = false;                     /* reconfigure heart-rate mode afterwards */
            continue;
        }
        if (!ok && (int32_t)(now_ms() - retry_at) >= 0) {
            ok = mode_setup(&rails_on) == ESP_OK;
            if (!ok) {
                ESP_LOGW(TAG, "MAX30001 not responding; heart rate unavailable, retrying in %d s",
                         HR_RETRY_MS / 1000);
                retry_at = now_ms() + HR_RETRY_MS;
            }
        }
        if (ok && s_stream_rate) {
            stream_service();
            if (s_irq_ok) {
                gpio_intr_enable(BOARD_PIN_ECG_INT);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STREAM_POLL_MS));
            continue;
        }
        if (ok) {
            poll_rr();
        }
        /* Wakes early when a capture or stream is requested, or on shutdown. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HR_POLL_MS));
    }

    intb_setup(false);
    if (s_cap.active) {
        capture_finish(ESP_ERR_INVALID_STATE);
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
    if (s_stream_rate && (s_stream_rate != ECG_RATE_HZ || req->test_signal)) {
        return ESP_ERR_INVALID_STATE;   /* the running stream cannot provide this capture */
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

/* Run the stream at the SD stream's rate, or 512 sps for the live tap alone, or not at all.
 * The AFE is only reconfigured when that rate changes. */
static void update_rate(bool force)
{
    uint16_t want = s_rec_rate ? s_rec_rate : (s_live_cb ? ECG_RATE_HZ : 0);
    if (want == s_stream_rate && !force) {
        return;
    }
    s_stream_rate = want;
    s_stream_changed = true;
    if (s_task && !s_task_done) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t ecg_stream_start(uint16_t rate_hz, ecg_stream_cb_t cb, void *ctx)
{
    if ((rate_hz != 512 && rate_hz != 256) || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    s_stream_ctx = ctx;
    s_stream_cb = cb;
    s_rec_rate = rate_hz;
    update_rate(true);
    return ESP_OK;
}

void ecg_stream_stop(void)
{
    s_stream_cb = NULL;
    s_rec_rate = 0;
    update_rate(true);
}

esp_err_t ecg_live_start(ecg_live_cb_t cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    s_live_ctx = ctx;
    s_live_phase = 0;
    s_live_cb = cb;
    update_rate(false);
    return ESP_OK;
}

void ecg_live_stop(void)
{
    s_live_cb = NULL;
    update_rate(false);
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
