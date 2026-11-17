# Plan: remote settings and logs for the SPRO sensor (AWS IoT)

Written 2026-11-16. Status (2026-11-17): **phases 1 and 2 done and verified on 0840E8.** Next:
phase 3. The other boards (15B0B4, 236B2C) get their identity the first time they charge on Wi-Fi
with the current firmware.

## Goal

- Change device settings remotely (automatic ECG, IMU and, later, GNSS file logging) and see what
  the device actually applied.
- See what a device is doing for remote debugging: health facts, a live log while charging, and a
  stored log of each session.

## Decisions made

| Decision | Choice | Why |
|---|---|---|
| Mechanism | **AWS IoT Device Shadow** over MQTT, from the device itself | Shows desired vs applied, works with the device offline, reuses the gateway's code, same connection later gives firmware updates (IoT Jobs) |
| When | **Charge-only mode**, on Wi-Fi, once per 10-minute round after the charging report and uploads | The only time the sensor has Wi-Fi; no battery cost |
| VPro's way (`setup` from the firmware-check endpoint) | **Not used** | One-way, no confirmation, user wants a fresh design |
| ESP Insights | **Not used** | No live log, no settings, RTC storage lost at battery power-off, needs a coredump partition, public service is evaluation-only, data in Espressif's cloud |
| Existing HTTPS calls (charging report, file uploads) | **Unchanged** | They work; this plan adds to them |
| Settings file | `config.json` on the SD card stays the one place the firmware reads at start-up | Shadow changes are written into it; they take effect at the next power-on |

## Design

**Identity.** Fleet provisioning by claim, as `equi-gateway-esp32` does (`gw_cloud.c`,
`gw_identity.c`): P-256 key made on the device, CSR sent, certificate + thing name stored in NVS.
Thing name `equi-sensor-<backend id>`, e.g. `equi-sensor-DCB4D9FFFE0840E8`. Separate policy,
claim policy, claim certificate and template from the gateway's, so a sensor can never publish as
a gateway. The claim key is inside the app image, which flash encryption covers on these boards.

**Shadow.** Named shadow `config`, mirroring `config.json`:

```json
{
  "state": {
    "desired":  { "ecg": {"log": false, "rate_hz": 256},
                  "imu": {"log": false, "rate_hz": 104},
                  "gnss": {"log": false},
                  "debug": {"live_log": false} },
    "reported": { "ecg": {...}, "imu": {...}, "gnss": {...}, "debug": {...},
                  "fw": "0.1.0", "hw": "SPRO-PCBA", "reset_reason": 4, "resumes": 1,
                  "battery": 98, "sd_free_mb": 28878, "files_pending": 0 }
  }
}
```

Each round: connect, `get` the shadow, apply any difference to `config.json` (validated with the
same rules as `storage.c`), publish `reported`, disconnect. A swapped SD card then reports its own
settings and gets the desired ones re-applied at the next charge.

**Topics** (all under the device's own thing only):
- `$aws/things/<thing>/shadow/name/config/get`, `/get/accepted`, `/update`, `/update/delta`
- `equimetrics/sensor/<thing>/log` (phase 4)

**Code.** New component `equi-sensordevice-vpro/components/iot/`, adapted from the gateway's
identity and MQTT code; called from `cloud_task` in `components/cloud/cloud.c`. Config writing goes
through a new `storage_config_save()` in `components/storage/storage.c`.

## Phases

### Phase 1: identity -- DONE 2026-11-17
Result on 0840E8: NVS had 309 of 504 entries free; provisioning took ~4 s (certificate issued,
thing `equi-sensor-DCB4D9FFFE0840E8`), the identity uses 46 NVS entries. AWS pieces as named
below, steps in `cloud/README.md` "Sensor identity". The sensor policy already covers the
shadow, IoT Jobs and the log topic.

1. **Measure free NVS space** on a board (`nvs_get_stats`) before anything is written. The
   partition is only 16 KB (SPro data, BLE bonds, Wi-Fi networks); the certificate + key need about
   1.5 KB. **Never erase NVS.** If it does not fit, stop and rethink (e.g. fewer BLE bonds).
2. AWS console (user; exact JSON to be written into `cloud/README.md` first):
   - policy `equimetrics-sensor` (connect as its own thing; its own shadow and log topic only);
   - claim policy `equimetrics-sensor-claim` and a claim certificate, saved to
     `private/sensor-claim/` (git-ignored);
   - fleet provisioning template `equimetrics-sensor-fleet`.
3. Firmware: `iot` component, provision on first connection, connect as the thing.
- **Done when:** the thing appears in IoT Core and the log shows an MQTT connection as that thing.

### Phase 2: settings -- DONE 2026-11-17
Result on 0840E8: the device created its shadow; `desired.imu.log = true` set in the console was
applied at the next round (`config.json` rewritten, `reported` matched), and the next session
recorded and uploaded an IMU file. Found and fixed on the way: the charging task used to end at
"battery full", which stopped the rounds; it now keeps them for the settings. `reported` already
carries `fw`, `hw`, `sd_card`, `reported_at`. Not exercised: an invalid value, no SD card.
Code: `components/iot` (`iot_sync`, `shadow_sync`), `storage_config_update` /
`storage_config_to_json`, callbacks in `components/cloud/cloud.c`.

- Shadow get / delta -> `config.json` -> reported.
- **Done when:** IMU logging is switched on in the AWS console's shadow editor, the device applies
  it at its next charging round, `reported` matches, and the next session writes an IMU file.

### Phase 3: health in the reported state
- Reset reason, resume counter (from the RTC `s_keep` data in `main.c`), firmware version, SD free
  space, files waiting to upload, battery.
- **Done when:** a forced crash (`CRASH_TEST_HOLD_MS`) shows up in `reported` after the next charge.
  Note: RTC data is lost at a battery power-off, so the counter must be saved to the SD card or
  NVS at shutdown to survive that.

### Phase 4: live log while charging
- `esp_log_set_vprintf` hook -> ring buffer in PSRAM -> batched publish (QoS 0) to the log topic
  while `debug.live_log` is true; the device then stays connected instead of disconnecting each
  round. Drop lines when the buffer is full; never log from inside the hook.
- **Done when:** the log is readable in the AWS console's MQTT test client.

### Phase 5: black-box session log
- The same hook writes warnings, errors and key events to a log file in the session folder on the
  SD card; uploaded at the next charge (route to decide: the data API's `api/imu` path, as VPro
  does for syslogs, or S3 through IoT).
- **Done when:** a session's log is retrievable after charging.

### Later, on the same connection
- `gnss.log` wired up once GNSS logging exists (accepted and stored from phase 2 on).
- Live log during a session (Wi-Fi on while running, as a debug setting: costs battery).
- Firmware updates (OTA), while charging. Two ways on this connection:
  - **Through the shadow** (simplest, fine for a handful of boards): `desired.fw` =
    `{version, url, sha256}`; the device downloads with `esp_https_ota`, restarts, and reports the
    running version. Weak point: a pre-signed S3 URL can expire before an offline device next
    charges.
  - **Through AWS IoT Jobs** (built for this): staged rollouts, per-device status, retries, and a
    fresh pre-signed URL per device at download time. Move to it when the fleet grows.
  - Either way on these boards: the image must be signed with the **spro** key (secure boot checks
    it); writes are encrypted automatically; rollback is already on (`confirm_ota_image`). The
    first OTA from the factory slot goes to **ota_0, which overwrites the original SPro app** (the
    recovery path in README "Build and flash"); the factory slot stays as the fallback. Purple LED
    is reserved for it.
- A settings screen in the web app (the backend calls `UpdateThingShadow`).
- The gateway's own settings (its README phase 2) with the same shadow layout.

## Risks and open points

- **NVS space:** fine on 0840E8 (263 entries free after provisioning). The figure is logged every
  round (`iot: NVS: ...`), so check it on 15B0B4 and 236B2C when they first run this firmware.
- **Device key in plain NVS:** NVS is not encrypted on these boards (`CONFIG_NVS_ENCRYPTION` off).
  Same exposure as today's Wi-Fi passwords; reading it needs the flash chip. Encrypting NVS means
  burning an eFuse key (permanent): a production decision, not part of this plan.
- **Flashing rules still apply:** app only, encrypted, at 0x20000; user's OK before every flash
  (README "Build and flash").
- **0840E8** is the bench board for this work (Wi-Fi provisioned, on the 2026-11-16 build); its
  the dev backend knows its ID `DCB4D9FFFE0840E8` (IMU upload accepted 2026-11-17).
- **Memory:** MQTT with a client certificate runs after the HTTPS calls, not at the same time, and
  BLE is off in charge-only, so heap should not be a problem; check the high-water mark anyway.

## Also still open from the firmware review

See README "Next steps", item 10 (flash 15B0B4 and 236B2C, SD-logging checks, unfixed findings).
