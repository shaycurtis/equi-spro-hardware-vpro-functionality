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
 *
 * Firmware updates: AWS IoT Jobs (REMOTE_SETTINGS_PLAN.md, phase 6; AWS steps in cloud/README.md,
 * "Firmware updates"). After the shadow, each round:
 *   - a just-updated image that got this far (connected to AWS) is confirmed; until then the
 *     bootloader rolls it back at the next reset;
 *   - a job started before the restart whose version is now running is reported SUCCEEDED;
 *   - $aws/things/<thing>/jobs/start-next gets the next job. A document
 *       {"operation":"ota", "version":"0.2.0", "url":"https://..."}
 *     (the url normally an ${aws:iot:s3-presigned-url:...} placeholder, filled in by AWS) is
 *     downloaded with esp_https_ota after the MQTT connection is closed. The image's own version
 *     must equal "version", and secure boot checks its signature. Then iot_restart_required()
 *     turns true and the caller restarts the device into it.
 *   - Up to IOT_OTA_MAX_TRIES per job (a failed download or a rollback each count), then FAILED.
 * The job in progress (id, version, tries) is kept in NVS namespace "iot", key "ota".
 */
#include <stdbool.h>
#include "esp_err.h"

#define IOT_OTA_MAX_TRIES 3

struct cJSON;

/* How the shadow maps to the device, plus feedback (all called from iot_sync's task). */
typedef struct {
    /* Apply settings from the shadow's "desired" object (only called when there is one). */
    void (*apply)(const struct cJSON *desired);
    /* Add the device's current settings to `settings`. NULL: no shadow. */
    void (*settings)(struct cJSON *settings);
    /* Add status fields (firmware version, ...) to `reported`. May be NULL. */
    void (*status)(struct cJSON *reported);
    /* A firmware download is starting (e.g. the purple LED). May be NULL. */
    void (*updating)(void);
} iot_cfg_t;

/*
 * One round, called with Wi-Fi connected and the clock set (certificates are checked).
 * device_id: the backend ID, e.g. "DCB4D9FFFE0840E8". cfg: NULL = connect, confirm and jobs only.
 * ESP_OK: connected as this device's thing, the shadow is up to date and jobs were checked.
 * ESP_ERR_NOT_SUPPORTED: this build has no endpoint, or no claim certificate and no saved identity
 * (logged once). Other errors: this round failed; the next one tries again.
 */
esp_err_t iot_sync(const char *device_id, const iot_cfg_t *cfg);

/* True once a new firmware image has been written and set to boot: the caller should close its
 * files (unmount the SD card) and restart. */
bool iot_restart_required(void);
