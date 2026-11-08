#include "ecg.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "max30001.h"

#define RAIL_SETTLE_MS   600    /* 1.8 V LDOs + 32.768 kHz oscillator start-up */
#define POLL_MS          20     /* FIFO is 32 deep = 62 ms at 512 sps */

static const char *TAG = "ecg";
static volatile bool s_busy;
static volatile bool s_stop;
static ecg_request_t s_req;

static void rail(bool on)
{
    gpio_set_level(BOARD_PIN_VCC_CTRL, on ? 1 : 0);
}

static void ecg_task(void *arg)
{
    const uint32_t target = (uint32_t)s_req.seconds * ECG_RATE_HZ;
    int16_t block[ECG_BLOCK_SAMPLES];
    size_t nblock = 0;
    uint32_t total = 0, overflows = 0, block_start = 0;
    int32_t raw[32];
    esp_err_t status = ESP_OK;

    rail(true);
    vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));

    uint32_t info = 0;
    status = max30001_bus_init();
    if (status == ESP_OK) {
        status = max30001_probe(&info);
    }
    if (status == ESP_OK) {
        status = max30001_ecg_start(s_req.test_signal);
    }

    int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    while (status == ESP_OK && !s_stop && total < target) {
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
    }
    if (nblock) {
        s_req.on_block(block, nblock, block_start, s_req.ctx);
    }
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;

    max30001_stop();
    rail(false);
    ESP_LOGI(TAG, "capture done: %lu samples in %lld ms (%.1f sps), %lu FIFO overflows, status %s",
             (unsigned long)total, dt_ms, dt_ms ? total * 1000.0 / dt_ms : 0.0,
             (unsigned long)overflows, esp_err_to_name(status));

    s_busy = false;
    if (s_req.on_done) {
        s_req.on_done(status, total, overflows, s_req.ctx);
    }
    vTaskDelete(NULL);
}

esp_err_t ecg_start(const ecg_request_t *req)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!req->on_block || req->seconds == 0 || req->seconds > ECG_MAX_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_req = *req;
    s_stop = false;
    s_busy = true;
    if (xTaskCreate(ecg_task, "ecg", 4096, NULL, 6, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
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
    s_stop = true;
    for (int i = 0; i < 20 && s_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    rail(false);
}
