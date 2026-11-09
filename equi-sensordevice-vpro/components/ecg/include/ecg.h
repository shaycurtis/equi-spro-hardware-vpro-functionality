#pragma once
/*
 * MAX30001 owner: continuous heart rate (hardware R-to-R detector), an optional continuous
 * stream (SD recording, 512 or 256 sps) and on-demand ECG captures (512 sps) streamed in
 * fixed-size blocks to a callback.
 *
 * Samples are int16: the 18-bit AFE value >> 2, so 1 LSB = ECG_UV_PER_LSB uV
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

/* Start the ECG task: heart-rate monitoring runs continuously from then on. */
esp_err_t ecg_init(void);

/*
 * Heart rate over the last `window_s` seconds: 60 / median beat-to-beat interval.
 * *bpm = 0 if fewer than 3 plausible beats (electrodes off / not detecting).
 */
void ecg_hr_get(uint32_t window_s, uint8_t *bpm, uint8_t *beats);

/* Start a capture in the background. ESP_ERR_INVALID_STATE if one is already running. */
esp_err_t ecg_start(const ecg_request_t *req);

/* Ask a running capture to stop early (on_done still fires). */
void ecg_stop(void);

bool ecg_busy(void);

/* Continuous stream: called from the ECG task with every batch read from the FIFO (<= 32). */
typedef void (*ecg_stream_cb_t)(const int16_t *samples, size_t n, void *ctx);

/*
 * Stream samples continuously at 512 or 256 sps (can be called before ecg_init). Light sleep
 * continues between FIFO reads. While streaming, captures must be 512 sps electrode captures
 * (they tap the stream); other requests get ESP_ERR_INVALID_STATE.
 */
esp_err_t ecg_stream_start(uint16_t rate_hz, ecg_stream_cb_t cb, void *ctx);
void ecg_stream_stop(void);

/*
 * Live tap (Bluetooth live ECG): 256 sps raw 18-bit samples (sign-extended, 4x the int16
 * resolution), whatever else is running. The SD stream keeps its own rate: at 512 sps the tap
 * takes every other sample. With no SD stream, a 512 sps stream is started for the tap (so a
 * gateway capture still works). rr_ms is the beat-to-beat interval of a beat detected since
 * the last call (0 = none). Called from the ECG task: keep it short, never block.
 */
#define ECG_LIVE_RATE_HZ   256
typedef void (*ecg_live_cb_t)(const int32_t *raw18, size_t n, uint16_t rr_ms, void *ctx);
esp_err_t ecg_live_start(ecg_live_cb_t cb, void *ctx);
void ecg_live_stop(void);

/* Shutdown hook: stop a capture and make sure the AFE is unpowered. */
void ecg_shutdown(void);
