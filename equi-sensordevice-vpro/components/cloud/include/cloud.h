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
 * The URL and key come from <repo>/private/api_env.h (git-ignored, like VPro's env.xh):
 *   #define API_DATA_URL "https://..."
 *   #define API_DATA_KEY "..."
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CLOUD_PERIOD_S 600   /* as the VPro firmware's Wi-Fi retransmit period */

typedef struct {
    uint8_t battery_pct;
    float temperature_c;
    float humidity_pct;
    bool charging;           /* false = charge complete */
} cloud_status_t;

typedef struct {
    const char *device_id;   /* as the backend knows it, e.g. "15B0B4" */
    const char *hw_version;
    const char *fw_version;
    const char *last_update; /* ISO 8601 UTC */
    void (*read_status)(cloud_status_t *st);
    void (*set_time)(uint32_t epoch_utc);   /* after SNTP: set the RTC */
} cloud_cfg_t;

/* Start the charging-time cloud task (returns at once). cfg's strings must stay valid. */
esp_err_t cloud_charging_start(const cloud_cfg_t *cfg);
