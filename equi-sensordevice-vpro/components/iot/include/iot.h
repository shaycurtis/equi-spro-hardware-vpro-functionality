#pragma once
/*
 * The sensor's own connection to AWS IoT Core (REMOTE_SETTINGS_PLAN.md at the repository root),
 * used while charging on Wi-Fi, after the backend calls: settings through the Device Shadow,
 * later the live log and firmware updates (IoT Jobs).
 *
 * Identity: AWS IoT fleet provisioning by claim, as the ESP32 gateway (equi-gateway-esp32):
 * the first time, the device makes a P-256 key, sends a CSR over a connection opened with the
 * claim certificate built into the firmware (<repo>/private/sensor-claim/, git-ignored), and
 * gets back its own certificate and thing name "equi-sensor-<backend id>". Those are kept in NVS
 * (namespace "iot", our own; nothing else in NVS is touched and NVS is never erased). The private
 * key never leaves the device. AWS set-up: cloud/README.md, "Sensor identity".
 *
 * Settings: the named shadow "config" of the thing.
 *   desired   what the device should use; edited in the AWS console (later the web app)
 *   reported  what the device uses, plus status fields; written by the device every round
 * Each round the device gets the shadow, applies the valid part of "desired", and publishes
 * "reported". A shadow with no "desired" yet (a new device) gets the device's current settings as
 * its desired state, so the fields are there to edit. Anything the device did not apply (an
 * invalid value, no SD card) stays visible as a difference between the two.
 */
#include "esp_err.h"

struct cJSON;

/* How the shadow maps to the device (all three are called from iot_sync's task). */
typedef struct {
    /* Apply settings from the shadow's "desired" object (only called when there is one). */
    void (*apply)(const struct cJSON *desired);
    /* Add the device's current settings to `settings`. */
    void (*settings)(struct cJSON *settings);
    /* Add status fields (firmware version, ...) to `reported`. May be NULL. */
    void (*status)(struct cJSON *reported);
} iot_shadow_t;

/*
 * One round, called with Wi-Fi connected and the clock set (certificates are checked).
 * device_id: the backend ID, e.g. "DCB4D9FFFE0840E8". shadow: NULL = connect only.
 * ESP_OK: connected as this device's thing and the shadow is up to date. ESP_ERR_NOT_SUPPORTED:
 * this build has no endpoint, or no claim certificate and no saved identity (logged once). Other
 * errors: this round failed; the next one tries again.
 */
esp_err_t iot_sync(const char *device_id, const iot_shadow_t *shadow);
