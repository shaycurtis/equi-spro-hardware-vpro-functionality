#pragma once
/*
 * On-demand ECG capture. Powers the MAX30001 rails (VCC_CTRL) only for the duration of a
 * capture, streams samples in fixed-size blocks to a callback, then powers everything off.
 *
 * Samples are int16 at 512 sps: the 18-bit AFE value >> 2, so 1 LSB = ECG_UV_PER_LSB uV
 * (~0.38 uV at gain 80) and full scale is about +-12.5 mV.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ECG_RATE_HZ        512
#define ECG_BLOCK_SAMPLES  112         /* 224 bytes + 6-byte header: one notification at MTU >= 233 */
#define ECG_UV_PER_LSB     (4.0 * 1e6 / (131072.0 * 80))
#define ECG_MAX_SECONDS    60

typedef void (*ecg_block_cb_t)(const int16_t *samples, size_t n, uint32_t first_index, void *ctx);
typedef void (*ecg_done_cb_t)(esp_err_t status, uint32_t total_samples, uint32_t overflows, void *ctx);

typedef struct {
    uint16_t seconds;
    bool test_signal;          /* internal 1 Hz +-0.5 mV square wave instead of electrodes */
    ecg_block_cb_t on_block;   /* called from the ECG task */
    ecg_done_cb_t on_done;     /* called from the ECG task once, after power-down */
    void *ctx;
} ecg_request_t;

/* Start a capture in the background. ESP_ERR_INVALID_STATE if one is already running. */
esp_err_t ecg_start(const ecg_request_t *req);

/* Ask a running capture to stop early (on_done still fires). */
void ecg_stop(void);

bool ecg_busy(void);

/* Shutdown hook: stop a capture and make sure the AFE is unpowered. */
void ecg_shutdown(void);
