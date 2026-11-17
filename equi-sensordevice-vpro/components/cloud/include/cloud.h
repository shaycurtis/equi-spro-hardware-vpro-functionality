#pragma once
/*
 * Backend ("cloud") work while the device charges on USB, against the VPro platform's API
 * (see README "VPro backend API"): Wi-Fi to a saved network, time sync, then HTTPS calls with
 * the data API key. Server certificates are checked (ESP-IDF certificate bundle).
 *
 * First step: the charging telemetry event, POST {API_DATA_URL}/api/sessions/events/charging,
 * the same JSON as the VPro firmware's VPro_Charging profile:
 *   { "deviceId", "sessionId" (= the send time, Unix ms), "timeStamp" (Unix ms),
 *     "device": { "hwVersion", "fwVersion", "lastUpdate", "batteryLevel", "temperature",
 *                 "humidity", "chargerStatus": "Charging" | "ChargingComplete" },
 *     "wifi":   { "ssid", "fileProgress": [] } }
 * sent every CLOUD_PERIOD_S while charging and once more when the battery is full.
 *
 * Then the SD card recordings (/sdcard/sessions/<session>/ecg_… / imu_….edf), as VPro's
 * FileUploader: ECG via POST api/v2/ecg/prepare -> PUT to the pre-signed URL -> POST
 * api/v2/ecg/<id>/process; IMU via POST api/imu -> PUT. Each file is deleted after a successful
 * upload; failures are retried (3 attempts per connection).
 *
 * Retries: each saved network is tried twice per round. A failed round (no Wi-Fi, no time, or the
 * backend not answering) is retried after 30 s, 1, 2, then every 5 min; recordings still on the
 * card after a round are retried after 1 min. A clean round waits CLOUD_PERIOD_S. Once the
 * battery is full, that was reported and the card holds no recordings, the charging reports stop
 * but the rounds go on for the settings.
 *
 * Last in each round: AWS IoT (components/iot), the device's own connection for its settings
 * (Device Shadow "config" <-> config.json on the card; see storage.h and iot.h).
 *
 * The URL and key come from <repo>/private/api_env.h (git-ignored, like VPro's env.xh):
 *   #define API_DATA_URL "https://..."
 *   #define API_DATA_KEY "..."
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CLOUD_PERIOD_S 600   /* as the VPro firmware's Wi-Fi retransmit period */

/* What the status LED (D1) should show while charging. */
typedef enum {
    CLOUD_SHOW_NONE = 0,     /* nothing to show (no Wi-Fi saved): D1 back to the charge colour */
    CLOUD_SHOW_CONNECTING,   /* joining Wi-Fi */
    CLOUD_SHOW_UPLOADING,    /* sending recordings */
    CLOUD_SHOW_SYNCED,       /* connected, report sent, nothing left to upload */
    CLOUD_SHOW_FAILED,       /* Wi-Fi or the backend failed: retrying */
    CLOUD_SHOW_UPDATING,     /* downloading new firmware (AWS IoT Jobs), then a restart */
} cloud_show_t;

typedef struct {
    uint8_t battery_pct;
    float temperature_c;
    float humidity_pct;
    bool charging;           /* false = charge complete */
} cloud_status_t;

typedef struct {
    const char *device_id;   /* as the backend knows it, e.g. "50787DFFFE15B0B4" */
    const char *hw_version;
    const char *fw_version;
    const char *last_update; /* ISO 8601 UTC */
    void (*read_status)(cloud_status_t *st);
    void (*set_time)(uint32_t epoch_utc);   /* after SNTP: set the RTC */
    void (*show)(cloud_show_t state);       /* optional: LED feedback */
} cloud_cfg_t;

/* Start the charging-time cloud task (returns at once). cfg's strings must stay valid. */
esp_err_t cloud_charging_start(const cloud_cfg_t *cfg);
