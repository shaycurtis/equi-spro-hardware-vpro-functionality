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

### Phase 6: firmware updates through AWS IoT Jobs (started 2026-11-17)
Decided 2026-11-17: Jobs, not the shadow (groups, rollouts, abort rules, per-device status, and a
fresh pre-signed download link made when the device fetches the job).
- **Versioning:** the firmware version comes from git: tag `sensor-v<x.y.z>` -> `x.y.z`; commits
  after a tag -> `x.y.z-N-g<hash>`; no tag yet -> `0.1.0-g<hash>`; `-dirty` for uncommitted changes.
  It is what `reported.fw`, the charging report and the job document compare.
- **Device, each charging round** (after the shadow): confirm a just-updated image once it reaches
  AWS (else the bootloader rolls it back at the next reset); report SUCCEEDED for a job whose
  version is now running; `start-next` gets the next job; a job document
  `{"operation":"ota","version":"<x.y.z>","url":"${aws:iot:s3-presigned-url:https://<bucket>.s3.eu-west-1.amazonaws.com/<key>}"}`
  is downloaded with `esp_https_ota` (the new image must carry that version; secure boot checks
  its signature), then the device restarts into it. Up to 3 tries per job (a failed download or a
  rollback counts), then FAILED. Purple LED while updating.
- **AWS (user):** a private S3 bucket for images, a role that lets IoT pre-sign links to it, static
  thing groups (`sensors-bench` first, then `sensors-pilot`, `sensors-fleet`), jobs targeting a
  group with rollout / abort / time-out settings. Steps: `cloud/README.md`, "Firmware updates".
- **Done when:** 0840E8 updates itself from a job, reports SUCCEEDED, and `reported.fw` shows the
  new version; then the same with all three boards in `sensors-pilot`.
- **Notes:** the first update from the factory slot overwrites the original SPro app in `ota_0`
  (factory stays as the fallback). The board's IDF 5.2.1 bootloader is assumed to have rollback on
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in the helloworld project it came from). A device
  that is restarted (power button) before its first round after an update rolls back; the job then
  retries. Later: dynamic groups by `reported.fw` (needs fleet indexing), AWS code signing, a
  screen in the web app, the gateway on the same mechanism.
- **Status (parked 2026-11-17):** code written and built (signed with spro), not yet flashed or
  tested; the AWS side (bucket, presign role, `sensors-bench`, job) not yet set up.
- **To decide / do before the first test:**
  1. DONE 2026-11-18: the user accepts that the first update overwrites the original SPro app in
     `ota_0` (left over from the original firmware's OTA; it can be reflashed if ever needed).
  - AWS side DONE 2026-11-18 (user): bucket `equimetrics-sensor-firmware-dev`, role
    `equimetrics-iot-jobs-presign` (inline `sensor-firmware-read` only; the console's three managed
    IoT policies removed), static group `sensors-bench` with `equi-sensor-DCB4D9FFFE0840E8`.
  2. Cable flashing after an update: once otadata points at `ota_0`/`ota_1`, the bootloader ignores
     `factory`, so an app flashed to 0x20000 does not run. Fix: also write `build\ota_data_initial.bin`
     encrypted at 0x320000 (otadata is encrypted on this board), as for a first VPro flash; NVS
     untouched. DONE: README "Rules for the SPRO board".
  3. DONE: each round logs `running from <slot>, OTA state <state>`: `pending verify` after the first
     update proves the 5.2.1 bootloader has rollback; `new` / `undefined` means it does not (updates
     still work, but a broken image would not roll back by itself). IDF 6.1 for the app does not
     change this: the bootloader decides, and the otadata format is the same in 5.2.1 and 6.1.
- **First test, 2026-11-18, PASSED on 0840E8:** cable-flashed `0.1.0-g25919da` (factory); job
  `sensor-fw-0-2-0-bench` (snapshot, group `sensors-bench`, pre-signed link) picked up at the
  next round, IN_PROGRESS, 1.6 MB written to `ota_0` in 71 s over Wi-Fi at -81 dBm, restart, first
  round on `0.2.0`: charging report and `reported.fw` show `0.2.0`, job SUCCEEDED.
  **Finding:** after the update the log said `running from ota_0, OTA state new`, not `pending
  verify`, so the board's IDF 5.2.1 bootloader was built WITHOUT rollback. Updates work, but a
  broken image is not rolled back by the bootloader. It falls back to another slot only when an
  image fails to load (bad signature, corrupt), not when the app crashes or cannot reach AWS.
  The bootloader cannot be replaced (secure boot rule: never flash a bootloader).
- **Test plan:** commit, flash that build by cable (version `0.1.0-g<hash>`); tag the same commit
  `sensor-v0.2.0` and build (version `0.2.0`); upload to the bucket; create a job for
  `sensors-bench` (0840E8 only); watch the next charging round update, restart, confirm, and the
  job go SUCCEEDED.

### Later, on the same connection
- `gnss.log` wired up once GNSS logging exists (accepted and stored from phase 2 on).
- Live log during a session (Wi-Fi on while running, as a debug setting: costs battery).
- Firmware updates: now phase 6 above.
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
