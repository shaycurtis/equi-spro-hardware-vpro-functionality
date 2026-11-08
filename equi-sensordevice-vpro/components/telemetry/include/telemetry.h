#pragma once
/*
 * Telemetry adverts, format version 5.
 *
 * Sent as BLE legacy, connectable advertising (1M PHY) so any scanner or commercial
 * listen-only gateway can forward it. Both records are Manufacturer Specific Data (type 0xFF)
 * with company ID 0xFFFF (Bluetooth SIG "test / not assigned" - replace with an Equimetrics
 * company ID for production).
 *
 * ADVERT - live data, every 30 s (20 bytes after the company ID, little-endian unless noted):
 *   off len field
 *    0   1  magic        0xE9
 *    1   1  version      0x05
 *    2   3  device_id    24-bit, BIG-endian (so 0x15B0B4 reads as "15B0B4")
 *    5   1  session_tag  low byte of the session ID: tells the gateway which session (from the
 *                        encrypted identity below) this reading belongs to
 *    6   1  seq          increments every sample (gateway de-duplicates on session + seq)
 *    7   1  flags        bit0 USB present, bit1 charging, bit2 charge full, bit3 charger fault,
 *                        bit4 RTC time valid, bit5 RFID scan in progress,
 *                        bit6 no trusted gateway heard us recently (dev gateway pairs on this)
 *    8   2  battery_soc  1/256 % (MAX17048 resolution), 0xFFFF = n/a
 *   10   2  battery_mv   mV, 0 = n/a
 *   12   2  temp         int16, 0.01 degC, 0x7FFF = n/a
 *   14   1  humidity     0.5 %RH steps (0..200), 0xFF = n/a
 *   15   4  steps        uint32 since power-on, 0xFFFFFFFF = n/a
 *   19   1  heart_rate   bpm over the last 30 s (MAX30001 R-to-R, median), 0 = no beats
 *  (4 bytes spare in the 31-byte advert)
 *  v4 = 4-byte session ID at offset 5 and everything after shifted by 3. v3 = v4 without
 *  heart_rate. v2 = v3 with battery_soc as 1 byte of whole %.
 *
 * SCAN RESPONSE - encrypted identity (23 bytes after the company ID). Only gateways that scan
 * actively receive it; they forward it as opaque bytes and only Equimetrics software holding
 * the fleet key can read it (HorseID is never sent in the clear):
 *    0   1  magic        0xE9
 *    1   1  type         0x82 (encrypted identity, v2)
 *    2   3  device_id    BIG-endian (clear)
 *    5   4  nonce        random, new whenever the identity changes (clear)
 *    9  10  ciphertext   AES-128-CCM of: session ID (4, LE; Unix time the session started,
 *                        bit 31 = random because the RTC had no time) + horse_id (6, LE; ISO
 *                        11784 country << 38 | national ID, 0 = no tag)
 *   19   4  tag          CCM authentication tag (4 bytes)
 *   CCM: key = fleet key (16 bytes), nonce = device_id (3) + nonce (4), AAD = bytes 0..8.
 *  v1 (0x81, older firmware) had session + horse_id in the clear plus the device name.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TELEMETRY_COMPANY_ID   0xFFFF
#define TELEMETRY_MAGIC        0xE9
#define TELEMETRY_VERSION      5
#define TELEMETRY_PAYLOAD_LEN  20
#define TELEMETRY_IDENTITY     0x82
#define TELEMETRY_IDENTITY_LEN 23

#define TELEM_FLAG_USB         (1u << 0)
#define TELEM_FLAG_CHARGING    (1u << 1)
#define TELEM_FLAG_FULL        (1u << 2)
#define TELEM_FLAG_FAULT       (1u << 3)
#define TELEM_FLAG_RTC_VALID   (1u << 4)
#define TELEM_FLAG_RFID_SCAN   (1u << 5)
#define TELEM_FLAG_NO_GATEWAY  (1u << 6)   /* no trusted gateway heard us recently */

typedef struct {
    uint32_t device_id;     /* 24 bits used */
    uint32_t session;          /* only the low byte is sent (session_tag) */
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

/* Set the encrypted identity scan response (call at start-up and whenever the session / horse
 * changes). */
esp_err_t telemetry_ble_set_identity(uint32_t device_id, uint32_t session, uint64_t horse_id);

/* Stop advertising (shutdown). */
void telemetry_ble_stop(void);

/* True if a trusted gateway sent a scan request for one of our adverts within max_age_ms
 * (a scan request is answered with the identity scan response). Needs a gateway that scans actively; the advert
 * itself is plain legacy ADV_IND, so passive gateways still get all the data. */
bool telemetry_gateway_seen(uint32_t max_age_ms);

/* esp_timer time (us) of the last trusted-gateway scan request, -1 = never. */
int64_t telemetry_gateway_last_seen_us(void);

/* Remember the peer on this connection as a trusted gateway (called when it writes a
 * command). Stored in NVS, up to 4, most recent first. */
void telemetry_trust_peer(uint16_t conn_handle);

/* True once at least one gateway has been learned. */
bool telemetry_has_trusted_gateway(void);

/* Total gateway scan requests since boot (for logging). */
uint32_t telemetry_scan_requests(void);

/* Log each distinct scanner seen since the last call (address, type, count), then reset. */
void telemetry_log_scanners(void);
