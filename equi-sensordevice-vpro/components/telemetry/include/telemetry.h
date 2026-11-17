#pragma once
/*
 * Telemetry adverts, format version 6.
 *
 * Sent as BLE legacy, connectable advertising (1M PHY) so any scanner or commercial
 * listen-only gateway can forward it. Both records are Manufacturer Specific Data (type 0xFF)
 * with company ID 0xFFFF (Bluetooth SIG "test / not assigned" - replace with an Equimetrics
 * company ID for production).
 *
 * ADVERT - live data, every 60 s (24 bytes after the company ID, little-endian unless noted):
 *   off len field
 *    0   1  magic        0xE9
 *    1   1  version      0x06
 *    2   3  device_id    24-bit, BIG-endian (so 0x15B0B4 reads as "15B0B4")
 *    5   1  session_tag  low byte of the session ID: tells the gateway which session (from the
 *                        encrypted identity below) this reading belongs to. The ID is Unix time
 *                        in ms, including the milliseconds. A multiple of 1000 has only 32
 *                        distinct low bytes (1000 and 256 share a factor of 8), so the tag would
 *                        otherwise repeat every 32 s.
 *    6   1  seq          increments every sample (gateway de-duplicates on session + seq)
 *    7   1  flags        bit0 USB present, bit1 charging, bit2 charge full, bit3 charger fault,
 *                        bit4 RTC time valid, bit5 RFID scan in progress,
 *                        bit6 no trusted gateway heard us recently (dev gateway pairs on this)
 *    8   2  battery_soc  1/256 % (MAX17048 resolution), 0xFFFF = n/a
 *   10   2  battery_mv   mV, 0 = n/a
 *   12   2  temp         int16, 0.01 degC, 0x7FFF = n/a
 *   14   1  humidity     0.5 %RH steps (0..200), 0xFF = n/a
 *   15   4  steps        uint32, steps since the previous reading (the first reading: since
 *                        power-on), 0xFFFFFFFF = n/a. v5 and older: total since power-on
 *   19   1  heart_rate   bpm over the last 60 s (MAX30001 R-to-R, median), 0 = no beats
 *   20   1  position     horse position, the VPro platform's numbering (0 unknown, 1 lateral,
 *                        2 sternal, 3 rolling, 4 standing, 5 moving): bits 0-2 = position now;
 *                        bits 3-7 = positions held since the last reading (bit 3 lateral,
 *                        4 sternal, 5 rolling, 6 standing, 7 moving). 0 = n/a
 *   21   2  horse_temp   int16, 0.01 degC, external DS18B20 probe on the 1-Wire connector pin,
 *                        0x7FFF = no probe. Optional: added to v6 without a version change, so
 *                        decoders that read 21 bytes still work; firmware before it sends 21
 *   23   1  health       optional, the same way: decoders that read 23 bytes still work;
 *                        firmware before it sends 23. bits 0-3 = reset reason of this boot
 *                        (esp_reset_reason_t, 0..15);
 *                        bits 4-7 = crash count, saturating at 15 (15 means 15 or more). A crash
 *                        is a panic, a watchdog, a brownout, a power glitch or a CPU lockup;
 *                        power-on and software restart do not add to it. The count is kept in
 *                        NVS. This byte fills the legacy advert (31 bytes on air).
 *  v5 = v6 without position. v4 = v5 with a 4-byte session ID at offset 5 and everything after shifted by 3. v3 = v4 without
 *  heart_rate. v2 = v3 with battery_soc as 1 byte of whole %.
 *
 * SCAN RESPONSE - encrypted identity (27 bytes after the company ID; the whole 31-byte scan
 * response is this one field). Only gateways that scan
 * actively receive it; they forward it as opaque bytes and only Equimetrics software holding
 * the fleet key can read it (HorseID is never sent in the clear):
 *    0   1  magic        0xE9
 *    1   1  type         0x83 (encrypted identity, v3)
 *    2   3  device_id    BIG-endian (clear)
 *    5   4  nonce        random, new whenever the identity changes (clear)
 *    9  14  ciphertext   AES-128-CCM of: session ID (8, LE; Unix time in ms the session
 *                        started, bit 52 = random because the RTC had no time) + horse_id (6,
 *                        LE; ISO 11784 country << 38 | national ID, 0 = no tag)
 *   23   4  tag          CCM authentication tag (4 bytes)
 *   CCM: key = fleet key (16 bytes), nonce = device_id (3) + nonce (4), AAD = bytes 0..8.
 *  v2 (0x82, older firmware) = v3 with a 4-byte session ID in seconds (bit 31 = random).
 *  v1 (0x81, older firmware) had session + horse_id in the clear plus the device name.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TELEMETRY_COMPANY_ID   0xFFFF
#define TELEMETRY_MAGIC        0xE9
#define TELEMETRY_VERSION      6
#define TELEMETRY_PAYLOAD_LEN  24
#define TELEMETRY_CRASH_MAX    15   /* high nibble of the health byte; 15 means this or more */
#define TELEMETRY_IDENTITY     0x83
#define TELEMETRY_IDENTITY_LEN 27

/*
 * Recordings the app starts with the ECG control service. start: ESP_OK = a file is being
 * written; ESP_ERR_NOT_SUPPORTED = the session already records ECG continuously (live view
 * only, no separate file); anything else = could not record (reported to the app as an
 * error). stop: keep = save the file, false = delete it ("discard"). Both are called from
 * NimBLE / timer context and must not block.
 */
typedef esp_err_t (*telemetry_rec_start_fn)(uint64_t recording_id, uint32_t seconds);
typedef void (*telemetry_rec_stop_fn)(bool keep);
void telemetry_set_recording_hooks(telemetry_rec_start_fn start, telemetry_rec_stop_fn stop);

/* BLE pairing passkey for the app's live services. Fixed for development; to be generated per
 * device at provisioning once the backend is connected. */
#define TELEMETRY_BLE_PASSKEY  111111

#define SESSION_RANDOM_BIT     (1ULL << 52)   /* session ID is random (RTC had no time); < 2^53 so JavaScript keeps it exact */

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
    uint32_t steps;            /* since the previous reading */
    uint8_t  heart_rate;       /* bpm, 0 = n/a */
    uint8_t  position;         /* 0..5 (VPro numbering), 0 = unknown */
    uint8_t  positions_seen;   /* bit n = position n held since the last reading */
    int16_t  horse_temp_c100;  /* external probe, 0.01 degC, 0x7FFF = no probe */
    uint8_t  reset_reason;     /* esp_reset_reason_t of this boot; low 4 bits are sent */
    uint16_t crash_count;      /* NVS total; the advert saturates it at TELEMETRY_CRASH_MAX */
} telemetry_t;

void telemetry_encode(const telemetry_t *t, uint8_t out[TELEMETRY_PAYLOAD_LEN]);

/* Remember this boot's reset reason and, if it was a crash, add one to the NVS counter.
 * Call once NVS is up (device_id_get opens it). A second call in the same boot does nothing. */
void telemetry_boot_health(void);
uint8_t telemetry_reset_reason(void);
uint16_t telemetry_crash_count(void);

/* Set the Bluetooth address (the chip's BT MAC + an offset, as VPro). Call before NimBLE starts;
 * telemetry_ble_init() does, and so does BLE provisioning, so both use the same address. Copies
 * the address to `mac_out` (may be NULL). False if the MAC could not be read. */
bool telemetry_ble_set_address(uint8_t mac_out[6]);

/* BLE: start NimBLE (device name e.g. "EQUI_15B0B4"). Non-blocking. */
esp_err_t telemetry_ble_init(const char *name);

/* Advertise `payload` for `duration_ms` at a 100 ms interval, replacing any burst in progress. */
esp_err_t telemetry_ble_burst(const uint8_t payload[TELEMETRY_PAYLOAD_LEN], int duration_ms);

/* Set the encrypted identity scan response (call at start-up and whenever the session / horse
 * changes). */
esp_err_t telemetry_ble_set_identity(uint32_t device_id, uint64_t session, uint64_t horse_id);

/* Stop advertising (shutdown). */
void telemetry_ble_stop(void);

/* True if a trusted gateway sent a scan request for one of our adverts within max_age_ms
 * (a scan request is answered with the identity scan response). Needs a gateway that scans actively; the advert
 * itself is plain legacy ADV_IND, so passive gateways still get all the data. */
bool telemetry_gateway_seen(uint32_t max_age_ms);

/* Per-device setup code (8 hex digits) derived from the fleet key: Wi-Fi provisioning proof of
 * possession and setup-network password. tools/setup_code.py computes the same value. */
void telemetry_device_setup_code(uint32_t device_id, char out[9]);

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
