#pragma once
/*
 * Telemetry adverts, format version 4.
 *
 * Sent as BLE legacy, connectable advertising (1M PHY; the gateway connects only for ECG /
 * time-set) so any scanner or commercial gateway can forward it. Both records are
 * Manufacturer Specific Data (type 0xFF) with company ID 0xFFFF (Bluetooth SIG "test / not
 * assigned" - replace with an Equimetrics company ID for production).
 *
 * ADVERT - live data, every 30 s (23 bytes after the company ID, little-endian unless noted):
 *   off len field
 *    0   1  magic        0xE9
 *    1   1  version      0x04
 *    2   3  device_id    24-bit, BIG-endian (so 0x15B0B4 reads as "15B0B4")
 *    5   4  session      Unix time (UTC) when this session started (device boot, or a new
 *                        horse scanned). Bit 31 set = random ID because the RTC had no time.
 *    9   1  seq          increments every sample (gateway de-duplicates on session + seq)
 *   10   1  flags        bit0 USB present, bit1 charging, bit2 charge full,
 *                        bit3 charger fault, bit4 RTC time valid, bit5 RFID scan in progress
 *   11   2  battery_soc  1/256 % (MAX17048 resolution), 0xFFFF = n/a
 *   13   2  battery_mv   mV, 0 = n/a
 *   15   2  temp         int16, 0.01 degC, 0x7FFF = n/a
 *   17   1  humidity     0.5 %RH steps (0..200), 0xFF = n/a
 *   18   4  steps        uint32 since power-on, 0xFFFFFFFF = n/a
 *   22   1  heart_rate   bpm over the last 30 s (MAX30001 R-to-R, median), 0 = no beats
 *  (1 byte spare in the 31-byte advert, reserved for respiration)
 *  v3 = v4 without heart_rate. v2 = v3 with battery_soc as 1 byte of whole %.
 *
 * SCAN RESPONSE - identity (15 bytes after the company ID) + complete local name "EQ-xxxxxx":
 *    0   1  magic        0xE9
 *    1   1  type         0x81 (identity record, v1)
 *    2   3  device_id    BIG-endian
 *    5   4  session      as above
 *    9   6  horse_id     ISO 11784: country (10 bits) << 38 | national ID (38 bits), LE.
 *                        0 = no tag scanned. Shown as 15 digits, e.g. 985141000123456.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TELEMETRY_COMPANY_ID   0xFFFF
#define TELEMETRY_MAGIC        0xE9
#define TELEMETRY_VERSION      4
#define TELEMETRY_PAYLOAD_LEN  23
#define TELEMETRY_IDENTITY     0x81
#define TELEMETRY_IDENTITY_LEN 15

#define TELEM_FLAG_USB         (1u << 0)
#define TELEM_FLAG_CHARGING    (1u << 1)
#define TELEM_FLAG_FULL        (1u << 2)
#define TELEM_FLAG_FAULT       (1u << 3)
#define TELEM_FLAG_RTC_VALID   (1u << 4)
#define TELEM_FLAG_RFID_SCAN   (1u << 5)

typedef struct {
    uint32_t device_id;     /* 24 bits used */
    uint32_t session;
    uint8_t  seq;
    uint8_t  flags;
    uint16_t battery_soc256;   /* 1/256 %, 0xFFFF = n/a */
    uint16_t battery_mv;
    int16_t  temp_c100;
    uint8_t  humidity_half;
    uint32_t steps;
    uint8_t  heart_rate;       /* bpm, 0 = n/a */
} telemetry_t;

void telemetry_encode(const telemetry_t *t, uint8_t out[TELEMETRY_PAYLOAD_LEN]);

/* BLE: start NimBLE (device name e.g. "EQ-15B0B4"). Non-blocking. */
esp_err_t telemetry_ble_init(const char *name);

/* Advertise `payload` for `duration_ms` at a 100 ms interval, replacing any burst in progress. */
esp_err_t telemetry_ble_burst(const uint8_t payload[TELEMETRY_PAYLOAD_LEN], int duration_ms);

/* Set the identity scan response (call at start-up and whenever the session / horse changes). */
esp_err_t telemetry_ble_set_identity(uint32_t device_id, uint32_t session, uint64_t horse_id);

/* Stop advertising (shutdown). */
void telemetry_ble_stop(void);
