#pragma once
/*
 * Maxim/ADI MAX30001 biopotential + bioimpedance AFE, ECG channel only (for now).
 * SPI on GPIO39-42, powered by the two XC6221 1.8 V LDOs switched by VCC_CTRL (GPIO13),
 * clocked by the external 32.768 kHz oscillator (also on the 1.8 V rail).
 *
 * ECG is configured for 512 or 256 sps, gain 80 V/V, digital HPF 0.5 Hz, LPF 40 Hz, with the
 * hardware R-to-R (heart-beat) detector enabled.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MAX30001_ECG_RATE_HZ   512
#define MAX30001_ECG_GAIN      80
/* uV per LSB of the full 18-bit sample: VREF(1 V) / (2^17 * gain) */
#define MAX30001_UV_PER_LSB18  (1e6 / (131072.0 * MAX30001_ECG_GAIN))

/* Bring up the SPI bus (once). Does not touch the AFE power. */
esp_err_t max30001_bus_init(void);

/* Reset + read INFO. Rail must already be on. Returns ESP_ERR_INVALID_RESPONSE if INFO looks wrong. */
esp_err_t max30001_probe(uint32_t *info);

/*
 * Configure and start ECG at 512 or 256 sps. test_signal: internal +-0.5 mV 1 Hz square wave,
 * electrodes isolated. fifo_irq_samples > 0: INTB (open drain, internal pull-up) goes low while
 * the ECG FIFO holds at least that many samples; 0 = no interrupt (polled).
 */
esp_err_t max30001_ecg_start(bool test_signal, uint16_t rate_hz, uint8_t fifo_irq_samples);

/*
 * Drain the ECG FIFO into `out` (18-bit signed samples). Returns the number read.
 * Sets *overflow if the FIFO overflowed (it is then reset; samples were lost).
 */
size_t max30001_ecg_read(int32_t *out, size_t max, bool *overflow);

/* If the R-to-R detector found a new beat since the last call, return the beat-to-beat
 * interval in ms. Cheap (two 32-bit SPI reads); poll it a few times per second. */
bool max30001_rtor_poll(uint32_t *rr_ms);

/* Discard the ECG FIFO (heart-rate-only mode does not read it). */
void max30001_fifo_reset(void);

/* Stop conversions and reset the chip (before the rail is switched off). */
void max30001_stop(void);
