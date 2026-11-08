#pragma once
/*
 * Telemetry advert, format version 1.
 *
 * Sent as BLE legacy, non-connectable advertising (1M PHY) so any scanner or
 * commercial gateway can forward it. The AD structure is Manufacturer Specific Data
 * (type 0xFF) with company ID 0xFFFF (Bluetooth SIG "test / not assigned" - replace
 * with an Equimetrics company ID for production).
 *
 * Payload after the company ID (19 bytes, little-endian unless noted):
 *   off len field
 *    0   1  magic        0xE9
 *    1   1  version      0x01
 *    2   3  device_id    24-bit, BIG-endian (so 0x15B0B4 reads as "15B0B4")
 *    5   1  seq          increments every sample (gateway de-duplicates on this)
 *    6   1  flags        bit0 USB present, bit1 charging, bit2 charge full,
 *                        bit3 charger fault, bit4 RTC time valid
 *    7   1  battery_soc  % (0..100), 0xFF = n/a
 *    8   2  battery_mv   mV, 0 = n/a
 *   10   2  temp         int16, 0.01 degC, 0x7FFF = n/a
 *   12   1  humidity     0.5 %RH steps (0..200), 0xFF = n/a
 *   13   4  steps        uint32 since power-on, 0xFFFFFFFF = n/a
 *   17   2  uptime       minutes since power-on (wraps at ~45 days)
 *
 * Future versions append fields (heart rate, respiration, ...) and bump `version`.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TELEMETRY_COMPANY_ID   0xFFFF
#define TELEMETRY_MAGIC        0xE9
#define TELEMETRY_VERSION      1
#define TELEMETRY_PAYLOAD_LEN  19

#define TELEM_FLAG_USB         (1u << 0)
#define TELEM_FLAG_CHARGING    (1u << 1)
#define TELEM_FLAG_FULL        (1u << 2)
#define TELEM_FLAG_FAULT       (1u << 3)
#define TELEM_FLAG_RTC_VALID   (1u << 4)

typedef struct {
    uint32_t device_id;     /* 24 bits used */
    uint8_t  seq;
    uint8_t  flags;
    uint8_t  battery_soc;
    uint16_t battery_mv;
    int16_t  temp_c100;
    uint8_t  humidity_half;
    uint32_t steps;
    uint16_t uptime_min;
} telemetry_t;

void telemetry_encode(const telemetry_t *t, uint8_t out[TELEMETRY_PAYLOAD_LEN]);

/* BLE: start NimBLE (device name e.g. "EQ-15B0B4"). Non-blocking. */
esp_err_t telemetry_ble_init(const char *name);

/* Advertise `payload` for `duration_ms` at a 100 ms interval, replacing any burst in progress. */
esp_err_t telemetry_ble_burst(const uint8_t payload[TELEMETRY_PAYLOAD_LEN], int duration_ms);

/* Stop advertising (shutdown). */
void telemetry_ble_stop(void);
