# Equimetrics VPro firmware (SPRO PCBA)

Custom firmware for the Equimetrics SPRO sensor board (ESP32-S3-WROOM-1-N16R8), a horse-worn
chest sensor, plus a PC gateway and dashboard for development. Status as of **2026-11-08, afternoon** (after the overnight test).

> **Signing keys never leave this PC.** `**/private/` and `*.pem` are git-ignored. All
> building and signing is local. The **fleet key** (`private/fleet_key.bin`, 16 bytes) is built into the
> firmware and read by the gateway to decrypt identities; same rule, and the build fails without it.

## Repository layout

| Path | What it is |
|---|---|
| `SPRO_PCBA/` | Altium schematics + PCB (source of truth for the hardware) |
| `HARDWARE.md` | Pin map, parts, power tree and hardware notes extracted from the PCB netlist |
| `equi-sensordevice-helloworld-fimware/` | Original IDF 5.2.1 reference project. **`private/` holds the signing keys** |
| `equi-sensordevice-blink/` | IDF 6.1 bring-up test (+ `recovery/otadata_boot_ota0.bin`, see below) |
| `equi-sensordevice-vpro/` | **The VPro device firmware** (secure boot + flash encryption) |
| `gateway-receiver/` | Firmware for a plain ESP32-S3 dev kit used as the BLE gateway radio |
| `gateway/` | PC gateway (Python) + local web dashboard, see `gateway/README.md` |

## Status

| Area | State |
|---|---|
| Toolchain, secure boot, flash encryption | ✅ IDF 6.1, signed with the **spro** key, app-only encrypted flashing |
| Power sequencing | ✅ Hold to power on (~1.5 s), long press to power off (2 s), charge-only on USB (light sleep, no reboots) |
| LEDs | ✅ D1 short blue blink every 5 s while on; D2 shows charge state (red/green/orange); both blink green while waiting for an RFID tag. Off on USB (charge-only): **D1 = charge**, **D2 = Wi-Fi / upload** (see Use, step 6). Purple is reserved for OTA updates |
| Wi-Fi provisioning | ✅ Espressif `network_provisioning` (SoftAP, security 1), **ESP SoftAP Provisioning** app; per-device setup code; up to 5 saved networks in NVS |
| Sensors (60 s) | ✅ Battery (MAX17048), temp/RH (SHTC3), steps (LSM6DSOX pedometer), RTC (PCF85063A) |
| GPS | ✅ Parked in backup mode at boot (`$PAIR650,0`, verified) |
| BLE telemetry | ✅ Advert v5 every 60 s (2 s burst, +15 dBm): battery (1/256 %), temp/RH, steps, heart rate, 1-byte session tag; **encrypted** identity scan response (AES-CCM, fleet key). Designed for listen-only commercial gateways |
| PC gateway + dashboard | ✅ SQLite, devices, sessions, charts, ECG panel |
| On-demand ECG | ✅ MAX30001 512 sps over BLE GATT, verified with real electrodes (74 bpm, ~15 µV noise) |
| Sessions + RFID HorseID | ✅ Session (Unix time **in ms**, as VPro) starts at the tag scan, or scan-button override (chip "Unknown"); JY-LD6900, 9600 ASCII; per-session dashboard view |
| Power optimisation (2e) | 🟡 Light sleep works, but the overnight run measured **~12-14 mA** (≈4.5-5 days on 1500 mAh) against a 3-6 mA estimate; with ECG 512 Hz + IMU 416 Hz logging **~28 mA** (≈2.2 days). Parked until the PPK2 is found (see below) |
| Heart rate | ✅ MAX30001 hardware R-to-R detector, median over 60 s, in advert v4 (verified 76-77 bpm on battery) |
| Respiration | ⏳ Planned (1 spare advert byte reserved) |
| SD card ECG logging (phase 3) | ✅ `/sessions/<session>/ecg_<device>_<chip>_<session>.edf`, 256 (default) / 512 Hz per `config.json`, mV (as VPro), interrupt-driven (light sleep kept), 0 overflows |
| Live ECG + app recordings | ✅ VPro-compatible BLE services (Raw ECG 256 sps, Heart Rate, ECG control); works with the **Equimetrics (VPro) app**; app recordings saved as their own EDF files. App needs a backend-registered **16-digit** ID (see "Device IDs" below); verified with `EQUI_CCBA97FFFE236B2C` |
| SD card IMU logging | ✅ `imu_…edf`: accel ±16 g + gyro ±2000 dps at 104/416 Hz, LIS2MDL mag at 104 Hz (sensor hub), FIFO + INT1, 0 overruns |
| Live BLE data to the backend | ✅ PC gateway `--mqtt` -> AWS IoT Core (`equimetrics/ble/<thing>/adv`) -> **postBleData** Lambda -> dev API -> Mongo -> app charts, one measurement a minute (`cloud/README.md`). `horseTemperature`, `respiration` (heart rate / 3), `position` and `positionsSeen` are **temporary placeholders** in the Lambda; `heartRate` is 0 when no beats were counted |
| Charging telemetry | ✅ On USB in charge-only mode: Wi-Fi + SNTP every 10 min, POST to the dev data API (in Mongo) |
| Wi-Fi file upload | ✅ In charge-only mode after the charging report, as VPro: ECG `prepare` -> PUT to the pre-signed URL -> `process` (IMU: `api/imu` -> PUT), file deleted after success, 3 tries per connection. Verified: a 39 s app recording, 20 KB in 0.25 s, all three steps 200 |
| Range test (50 m barn target) | ⏳ Not done yet (bench: ~18/20 adverts at -37 dBm) |

## Device behaviour (user view)

0. **Locked (no Wi-Fi saved):** power on and **both LEDs are steady white**: provisioning mode (below).
1. **Power on:** hold the power button ~1.5 s. Green double flash, then **both LEDs blink green**: scan the horse's tag.
2. **Tag read** starts the session and its SD recording. No tag? Press the **scan button** during the wait to start it anyway (chip "unknown"). After a 2 min timeout with neither, no session starts (press the scan button to scan again). Then: D1 **steady blue** until a trusted gateway has received the session (encrypted identity), then a short blink every 5 s, **green while a gateway is hearing it, blue if not**, D2 off (or charge colour on USB). The **scan button (SW1)** rescans; a different horse starts a new session.
3. **Running:** heart rate monitored continuously; every 60 s, sample sensors and send a BLE advert burst. The chip light-sleeps in between.
4. **Plugging in USB** while running on battery ends the session automatically (after 2 s): orange flashes, then charge-only (D2 red/green). Booting while already on USB keeps running (bench use).
5. **Live ECG (Equimetrics app):** the app lists the device, pairs (passkey **111111**, asked once per
   phone) and shows the live ECG; recordings started in the app are saved to the SD card (below).
6. **Power off:** hold the power button 2 s (orange, then 3 orange flashes). On battery the board is fully unpowered; on USB it goes to charge-only:
   - **D1 = charge:** red = charging, green = full, orange = charger fault.
   - **D2 = Wi-Fi / upload:** blue blinking = connecting, blue breathing = uploading recordings,
     steady blue = connected and everything sent, **orange flash every 5 s** = Wi-Fi or upload
     failed (retrying). No Wi-Fi saved: D2 shows the charge colour too.
   - Retries while charging: each saved network twice per round (30 s per try); a failed round again after 30 s,
     1 min, 2 min, then every 5 min; recordings still on the card after 1 min; otherwise every 10 min.
7. **Add a Wi-Fi network:** hold **both buttons** for 3 s, from off (power-on with both held) or while running: 3 white flashes, then steady white.

### SD card recording

- `/sdcard/config.json` (created with defaults if missing; later edited from the web app):
  `{"version":1,"ecg":{"log":false,"rate_hz":256},"imu":{"log":false,"rate_hz":104}}` (no SD
  logging by default: it roughly doubles the drain)
  (ECG 256/512 Hz; IMU 104/416 Hz for accel + gyro, magnetometer always 104 Hz). `ecg.log` is the
  **automatic (continuous) ECG** recording; off in normal use, where the app starts recordings.
  The defaults only apply when the file is missing: an existing card keeps its values.
- Each session writes `/sessions/<session>/<type>_<device id>_<chip number>_<session>.edf`, e.g.
  `/sessions/1794211982000/ecg_50787DFFFE15B0B4_999012345678436_1794211982000.edf` (session ID = Unix time in
  ms, chip `Unknown` without a tag: the VPro platform's naming, whose uploader reads the session from
  the folder and the other fields from the file name). ECG and IMU are separate files.
- **App recordings** (automatic ECG off): every `start` from the app writes its own ECG file in the
  session's folder, named after the recording's start time in ms (the app's recording ID):
  `ecg_<device id>_<chip>_<startMs>.edf` (folder = the recording ID if no session has started). Saved when
  the timer ends (`finished`) or on `stop`, deleted on `discard`, saved on power-off. With automatic
  ECG on and a session running, the session file already has the ECG: the app gets the live view only.
- Files are **EDF+C, as the VPro firmware writes them** (edflib; checked against a VPro file): EDF+
  patient `<chip|Unknown> X X X`, recording `Startdate dd-MMM-yyyy X X X`, plus an `EDF Annotations`
  signal (60 samples) whose time-keeping TAL gives each record's onset after the header's start second
  (e.g. `+0.026`), so the start is exact without padding: the file begins with the first real sample.
- ECG EDF: 0.5 s records (128 samples at 256 Hz), int16, 0.381 uV/LSB, **mV** (-12.5 .. 12.49962). Written every 60 s (app recordings every 5 s) with the record count updated and the file
  synced, so a battery cut loses at most that much. A different horse switches to a new session file
  at the exact sample.
- The MAX30001 interrupt (INTB, open drain to 1.8 V) wakes the chip every 16 samples; light sleep
  continues in between. Measured: exactly 512.0 sps, 32 reads/s, 0 overflows. Gateway ECG requests
  tap the same stream (512 Hz only); the app's live ECG takes 256 Hz from it (every other sample at
  512 Hz).
- IMU EDF: AccX/Y/Z (g, 0.488 mg/LSB), GyrX/Y/Z (dps, 70 mdps/LSB) at the IMU rate, MagX/Y/Z (uT,
  0.15 uT/LSB) at 104 Hz, raw sensor axes. The record duration is the IMU's real one from its
  oscillator trim (e.g. 0.99552 s at "416 Hz" = 417.87 Hz), so it stays aligned with the ECG's
  crystal timeline. Each file starts at its own first sample (its first TAL onset).
- The magnetometer includes the board's hard-iron offset (~(-124, -13, 19) uT on this unit, from the
  buzzer / motor / battery); calibrate it out in analysis (rotate through all orientations, subtract
  the centre). Measured: 25137 accel + gyro and 6285 mag samples per minute, 0 overruns.
- Check a file: `python tools/edf_info.py F:/sessions/<session>/ecg_…edf` (or open in EDFbrowser).
  Verified 2026-11-10: an app recording uploaded from 236B2C passes an EDF+ check (TAL onsets step by
  0.5 s) and matches the VPro file's layout field for field.
- Card: Transcend industrial 32 GB, 815 KB/s write over SPI3 at 20 MHz (ECG needs ~1 KB/s).

### Live ECG with the Equimetrics app (VPro-compatible BLE)

The board offers the same BLE services as the VPro firmware (`BLE_Local` in the VPro platform), so the
existing **Equimetrics app** works unchanged. Details: `components/telemetry/live_service.h`.

- **Services:** Raw ECG (256 sps, up to 10 samples of 16 bytes per notification: ms timestamp, raw
  18-bit value, status, RR interval), Heart Rate (0x180D, every 1 s), Respiration (present, not sent
  yet), ECG control (`start` / `stop` / `discard`, duration (cumulative), recording ID, status, total
  duration). The app's flow: set recording ID + duration, `start`, live view, `stop` / `discard`.
- **Pairing:** every characteristic needs an encrypted link (as VPro); we request MITM, so the phone
  asks for the passkey **111111** (`TELEMETRY_BLE_PASSKEY`; to be generated per device at
  provisioning once the backend is connected). Bonds are stored in NVS.
- **Advertising:** a second, connectable advert every 1 s for the app (VPro's layout: flags + Heart
  Rate + Raw ECG UUID, name in the scan response), next to the 60 s telemetry bursts. Name =
  `EQUI_` + the device ID (VPro: `BOARD_HOSTNAME_PREFIX` + its LoRa EUI-64), e.g.
  `EQUI_CCBA97FFFE236B2C`. The BT address is the chip's BT MAC + `BLE_ADDR_OFFSET` (VPro adds 1 to
  get past iOS's name cache; bump it after any name change; now 5).
- **The app only connects to registered 16-digit device IDs.** It lists any `EQUI_` name but says
  "It was not possible to connect with the device" without even connecting otherwise. A registered
  6-digit ID (`15B0B4`) was not enough; the 16-digit `CCBA97FFFE236B2C` works.
- **Device IDs:** the backend ID is an EUI-64 from the factory Wi-Fi MAC's OUI + `FFFE` + the SPro
  6-digit ID from NVS (`device_id_backend_get`): the BLE name, files, EDF header, charging telemetry
  and uploads use it. The BLE telemetry adverts carry only the 6 digits; postBleData rebuilds the
  full ID from the advert's MAC address (`backend_id`). Registered on dev (by hand):

  | Board | Backend ID |
  |---|---|
  | 15B0B4 | `50787DFFFE15B0B4` (still runs the 6-digit firmware until reflashed) |
  | 236B2C | `CCBA97FFFE236B2C` |
- Measured: 1529-1537 notifications/min (256 sps), 0 dropped at MTU 247; a phone reconnect mid-
  recording dropped 5 notifications while the MTU was renegotiated, the recording carried on.
- Not done yet: lead-off detection (status says "leads on" while beats are detected), respiration.

### Wi-Fi provisioning

Official Espressif flow, nothing custom: the `espressif/network_provisioning` component (SoftAP scheme,
security 1) with Espressif's **ESP SoftAP Provisioning** app (iOS / Android).

- The device opens a 2.4 GHz access point **`PROV_<device id>`** (e.g. `PROV_15B0B4`). Its WPA2 password
  and the app's proof of possession are the same per-device **setup code**: the first 8 hex digits of
  SHA-256(fleet key | "EQ-POP" | device id). `python tools/setup_code.py 15B0B4` prints it and the
  app's QR payload (needs `private/fleet_key.bin`).
- In the app: Provision Device → "I don't have a QR code" → join `PROV_…` → enter the code → pick the
  network. Both LEDs **flash white** while the device connects; a wrong password / missing network goes
  back to steady white for the app to retry.
- Success: the network is saved (NVS `vpro`/`wifi_nets`, up to 5, newest first) and the device **powers
  off** with the LEDs dark (on USB, charge-only then shows the charge colour). The next power-on is a
  normal session.
- Exit without finishing: hold the power button 2 s. A device that already has a network then starts
  normally next time; one with none stays locked. Provisioning mode times out after 10 min (power off).
- The saved networks are for the Wi-Fi-while-charging work (time, backend check-in, OTA, uploads):
  not used yet.

## Build and flash (device firmware)

Activate IDF 6.1 in PowerShell: `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`

```powershell
cd equi-sensordevice-vpro
idf.py build
espsecure verify-signature --version 2 --keyfile ..\equi-sensordevice-helloworld-fimware\private\spro\fw_signing_key.pem build\equi-sensordevice-vpro.bin
cd build
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before default-reset --after no-reset --no-stub write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB --encrypt 0x20000 equi-sensordevice-vpro.bin
```

**Rules for the SPRO board (secured; mistakes can be permanent):**
- Flash **only** the app, encrypted, at 0x20000 (factory slot). **Never** `idf.py flash` / `encrypted-flash`
  (they write a new bootloader). The board keeps its IDF 5.2.1 secure bootloader.
- Always pass `--flash-size 16MB`: in Secure Download Mode the ROM rejects addresses above ~2 MB otherwise.
- Sign with the **spro** key only. The board trusts one key digest (KEY0); slots 1 and 2 are revoked.
- **Never erase NVS**: it holds the SPro device ID (`15B0B4`), BLE passkey and settings.
- **A board still on the SPro app** (first VPro flash): also write `build\ota_data_initial.bin`
  encrypted to 0x320000 in the same command (blank OTA record, so the bootloader picks the factory
  slot instead of the SPro app in ota_0). Then: read the device ID from the boot log
  (`using SPro device ID from NVS: …`), have it registered in the dev DB, and provision Wi-Fi.
  Fit a **blank SD card** (the firmware does not wipe old SPro files; it only writes `config.json` if
  it is missing). Boards done so far: `15B0B4`, `236B2C`.
- Check the port first: `esptool -p COM3 --no-stub get-security-info` must say *Secure Boot: Enabled*.
- Back to the original SPro app (in ota_0): write `equi-sensordevice-blink/recovery/otadata_boot_ota0.bin`
  encrypted to 0x320000 (same esptool options).

**Receiver dev kit (COM4):** plain chip, no security. `cd gateway-receiver; idf.py -p COM4 flash`.
Check `get-security-info` says *Secure Boot: Disabled* first. Never flash VPro images there.

COM numbers can change: identify boards by `get-security-info` (SPRO CP210x serial `0001`).

## Run the gateway and dashboard

```powershell
python gateway\gateway.py --serial COM4 --web
```

Open http://localhost:8080 for device cards, sessions, charts and ECG capture. Details are in `gateway/README.md`.

## Protocols (source of truth in code)

- **Telemetry advert v5 + encrypted identity scan response:** `equi-sensordevice-vpro/components/telemetry/include/telemetry.h`
  (decoder: `gateway/equi_protocol.py`). Company ID `0xFFFF` is a **test ID**, so get an Equimetrics SIG ID for production.
- **ECG GATT service (gateway):** `equi-sensordevice-vpro/components/telemetry/ecg_service.h` (start/stop/set-time, 112-sample notifications).
- **Live ECG / app services (VPro-compatible):** `equi-sensordevice-vpro/components/telemetry/live_service.h`.
- **Identity v3** (0x83): 8-byte session ID in ms (bit 52 = random, so JavaScript keeps it exact); the
  gateway still reads v2 (4-byte seconds).
- **Receiver serial lines:** top of `gateway-receiver/main/receiver_main.c` (`EQADV`, `EQECG*`, `ECG,...` command).

## Gotchas learned (read before changing things)

- **FreeRTOS tick is 10 ms**: `pdMS_TO_TICKS(1)` is 0. Use `esp_rom_delay_us()` for sub-tick waits.
- **MAX30001**: the first SPI command after reset returns invalid data (dummy read after reset).
  SDO swings only to OVDD = 1.8 V, out of spec for ESP32 VIH but working; keep SPI at 4 MHz.
  Flag for the next board revision.
- **JY-LD6900 RFID**: tag frames are 9600 8N1 ASCII `$F<15 digits><BCC>#`, but its power-up tuning
  report is sent at **19200** (shows as garbage at 9600, safe to ignore). It reports a tag **once
  per field entry**, so if a scan misses it, move the tag away and back. Module is rated 3.6-5.5 V but
  fed from V_BATT (3.0-4.2 V); draws up to 250 mA, so only powered while scanning.
- **GPIO19/20** are the native-USB pins and carry I2C, so the USB-Serial-JTAG peripheral is disabled.
- **`SPIRAM_MALLOC_RESERVE_INTERNAL`** must stay small (64 KB); 256 KB from the old config boot-looped.
- **Serial monitor resets the board**: opening/closing COM3 toggles DTR/RTS and the auto-reset circuit
  reboots the device (it then falls into charge-only). Short sessions in the DB are often this, not a bug.
- **Light sleep pitfalls (fixed, keep in mind):** ESP-IDF floats every GPIO in light sleep unless
  `gpio_sleep_sel_dis()` is set, which would drop PMIC_EN and cut power; UARTs must be clocked from
  XTAL (APB scales with DFS); an enabled RMT channel blocks sleep; FreeRTOS ticks lag in light
  sleep, so use `esp_timer` for real-time deadlines and pulse timing. The WS2812 data line can
  glitch across a sleep/wake, so every frame is preceded by a full reset period.
- **No deep sleep in charge-only:** every deep-sleep wake is a reboot and the LEDs visibly blinked
  each time (holding the data pad low and skipping identical frames did not help). Charge-only now
  idles in automatic light sleep (~1-2 mA from USB) and restarts on a button press.
- **LSM6DSOX sensor hub (LIS2MDL):** only switch the hub's I2C master on/off with the accelerometer
  powered down (ST AN5272); otherwise the hub freezes (STATUS_MASTER stays 0) and stays frozen across
  an ESP reset, because the IMU stays powered on USB. Even done right, the first continuous start
  after configuring the LIS2MDL does not run, so the start is verified and retried. Init stops the
  hub before the software reset (a reset alone does not). A recorder signal that stops delivering
  is padded after 10 records instead of holding the whole file.
- **Buttons:** the power component configures the scan-button pad itself; an unconfigured pad reads 0
  ("pressed"), which once made every power-off in provisioning mode look like the both-buttons gesture.
- **RTC oscillator-stop flag** was seen once after the RFID reader switched on (coin cell is fitted).
  Firmware tolerates it (plausible date still used) and the gateway re-syncs time. Measure the coin cell
  if it recurs.
- **Gateway receipt (green heartbeat):** the advert is plain legacy ADV_IND (any gateway receives it),
  sent through NimBLE's extended-advertising API only so the controller reports scan requests. An
  actively scanning gateway sends one per advert it hears; that is the receipt. Other scanners do too
  (a smart-home hub here, `DC:73:06:…`, scans ~10x per burst), so only **learned gateways** count: a
  central that connects and writes a control command (time set, ECG, or `PAIR 0x20`) is stored in NVS
  (namespace `vpro`, up to 4, most recent first). When no trusted gateway has heard the device for
  ~65 s it sets advert flag bit 6 (`NO_GATEWAY`); the PC gateway then connects and sets the time,
  which pairs it (rate-limited to once per 2 min per device). Verified: pairs on the first burst,
  goes blue ~60-90 s after the receiver is unplugged, green again on the next burst when it returns.
  **Gateway decision (2026-11-08): off-the-shelf, listen-only gateways** (no BLE connections; we
  can't certify our own). Requirements: active scanning and forwarding scan responses (the encrypted
  identity is in the scan response). The connect-to-pair path above is a **dev shortcut** only; in
  production the trusted-gateway list will come from the SD card / Wi-Fi while charging. Live ECG at
  the barn is not required; later options: a phone app over BLE, or Wi-Fi streaming on request.
- **USB power-good blip at boot:** PWR_GD read low for one 250 ms poll as the RFID reader powered up,
  which looked like a battery -> USB plug-in and powered the device off (3 orange flashes). USB loss now
  has to last 1 s (`USB_LOSS_DEBOUNCE_MS`) before it counts.
- **BLE controller activities:** `CONFIG_BT_CTRL_BLE_MAX_ACT` was 3; two connectable advertising sets
  (telemetry + app) plus a connection exceed it and the telemetry bursts failed (`rc=519`). Now 8, with
  `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=2`.
- **iOS caches a device's name per address** and the app uses the cached one: after a name change the
  phone kept showing the old name. Bump `BLE_ADDR_OFFSET` so phones see a new device.
- **No GATT client:** the central role is off, so the device can't start an MTU exchange; the app does.
  At MTU 23 live notifications carry fewer samples (1) rather than being cut off.
- **Gateway DB**: readings are not unique by (seq, uptime); de-dup is in memory per burst. Session
  losses are only counted within one gateway run + one device session.

## Overnight test results (2026-11-08, firmware 4d51a5e)

- 03:44-16:12 UTC (12.5 h) on battery, worn on the chest. **1497/1497 readings received, no gaps**,
  ~17/20 adverts per burst, RSSI -58 to -73 dBm across the house.
- Heart rate valid in every reading while worn (median 63-72 bpm, falling through the night); a few
  single-reading outliers up to ~100 bpm. Garbage (31-157) only after the device came off.
- **Battery:** 98.8 % -> 86.9 % (4175 -> 4080 mV). Linear fit **0.95 %/h ≈ 14 mA**; 16 mA in the
  first 4 h, 11.5 mA in the last 4.5 h (the MAX17048 reads steeper near full). Call it 12-14 mA,
  i.e. about 4.5-5 days per charge. Expected 3-6 mA, so ~8 mA is unaccounted for.
- Suspects, in order: the two WS2812s' standby current (~1 mA each, powered all the time); main
  XTAL kept on in light sleep; how much time the chip really spends asleep (build with
  `CONFIG_PM_PROFILING` and dump `esp_pm_dump_locks()`); MAX30001 and GPS backup current.
- Fixes since: WS2812 frame shift after light-sleep wake (D1 stuck green until the next blink),
  and auto-stop on USB plug-in. Built, not yet flashed/verified at the time of writing.

## Overnight test with SD logging (2026-11-09)

- 02:44-07:34 UTC (4 h 50 min) on battery, ECG 512 Hz + IMU 416 Hz (+ mag 104 Hz) to the SD card.
  Gateway: 581/580 readings, no gaps. IMU file: 17512 complete records, matching the file size.
- **Battery:** 98.8 % -> 89.3 % (4175 -> 4098 mV): **1.96 %/h ≈ 28 mA** (1.83 %/h in the second half),
  vs 0.95 %/h without logging: logging roughly doubles the drain (~2.2 days per charge). Likely: SD card
  not dropping into its low-power idle in SPI mode, CPU wake-ups (32/s ECG + IMU FIFO), IMU in
  high-performance mode. To be measured with the PPK2.

## Battery drain investigation (parked 2026-11-08, waiting for the PPK2)

- **Profiling (`CONFIG_PM_PROFILING=y`, dumps every 60 s from the sample loop; off by default):** chip in
  light sleep **97.5 %** of the time, ~18 wake-ups/s, no rejected sleeps. Awake time (≈1.05 s/min at
  160 MHz + 0.45 s/min Bluetooth) is worth only ~1-1.5 mA, so of the overnight ~12-14 mA, **~11 mA is a
  constant board load present during sleep**.
- Suspects: (1) **the microSD card** (fitted; powered whenever the device is on, never initialised, its
  SPI pins float in light sleep); (2) the LC29H GNSS (always powered; how deep is `$PAIR650` backup?);
  (3) the two WS2812s (~1-2 mA, no power switch: board change needed); (4) ESP32 floor with the main
  XTAL on for BLE (~1 mA).
- **Test to run:** PPK2 in **source-meter mode at 3.8 V on the battery connector, battery removed, SPRO
  USB unplugged** (the BQ25303J has no power path, so USB would backfeed the PPK2). Read the 30 s average
  and the between-burst floor, then pull the SD card and repeat; optionally the off-state leakage.
- Firmware follow-ups once the culprit is known: SD card idle command at boot + defined pin levels in
  sleep; interrupt-driven heart rate (MAX30001 INTB on GPIO7) and button wake instead of polling.

## Next steps

1. **Live data soak test:** leave a session running through the PC gateway and watch for gaps or
   errors (gateway, postBleData logs in CloudWatch, Mongo). Reflash the **receiver dev kit** (its
   on-demand ECG wait is now 75 s for the 60 s bursts). Then: DynamoDB dedup for several gateways,
   a commercial gateway + adapter, postBleData memory 256 MB and its own role for production.
2. **Wi-Fi while charging, next:** ECG upload works (check the recording appears in the app); then
   the upload progress in the charging report (`fileProgress`), the device event log, firmware check
   + remote `setup` (config). Reflash 15B0B4 for the 16-digit ID.
3. **BLE bond store:** `ble_store_config_write_local_irk rc=27` at boot (store capacity); pairing works
   but check bonds survive reboots.
4. **Web Bluetooth config page:** edit `config.json` settings and view the saved networks from a
   browser (Chrome on Android / desktop; Bluefy on iOS).
5. **Power:** the parked PPK2 drain test (SD card is the top suspect for the extra ~8 mA), plus the
   cost of logging (ECG interrupt every 31 ms, IMU FIFO reads ~30/s at 416 Hz); then shorten the 2 s
   burst and tune TX power after the range test. The app advert (every 1 s) adds a little; measure it.
6. **Range test** on a horse in the barn (target 50 m): gateway `--summary 60`, walk the device out.
7. **Respiration** (MAX30001 BioZ or ECG-derived; the app's respiration service and the
   backend's `respiration` field, now a placeholder, are waiting for it), posture for `position`, and checking R-to-R detection on a horse ECG
   (large T waves may need WNDW/PTSF tuning).
8. Lead-off detection on the MAX30001 (status byte of the live ECG). **On hold (user decision,
   2026-11-10):** with the electrodes unattached the R-to-R detector reports noise as ~130 bpm (soak
   test), which reaches the backend; accepted for now. VPro's DC lead-off (100 nA, +/-300 mV, 25 MOhm
   bias) has wrongly reported leads off on a horse before, so it is not a quick fix.
9. Tidy-ups: quieten the per-burst scanner log; magnetometer hard-iron calibration in analysis.
10. Production items: SIG company ID, off-the-shelf listen-only gateway (active scan + scan
   responses), per-device keys and BLE passkey, AWS backend, test-lab check that +15 dBm stays
   within the SPRO certification.

## VPro backend API (from the VPro platform code, 2026-11-09)

Learned from `equimetrics-equi-sensordevice-platform` (IDF 5.2.1, LoRa; read only, not copied). HTTPS +
JSON, header `x-api-key`; two base URLs and keys (`WIFI_API_DATA_URL/KEY`, `WIFI_API_FW_URL/KEY`) from a
git-ignored `env.xh`.

| Purpose | Request |
|---|---|
| ECG upload slot | `POST {DATA}/api/v2/ecg/prepare` `{sessionId, deviceId, horseChipNumber, fileName, startedAt, endedAt}` -> `{id, uploadUrl}` |
| Upload | `PUT <uploadUrl>` (pre-signed, raw file bytes) |
| ECG processing | `POST {DATA}/api/v2/ecg/{id}/process` |
| IMU / GPS / syslog | `POST {DATA}/api/imu` -> `{id, uploadUrl}`, then PUT (no process step) |
| Charging telemetry | `POST {DATA}/api/sessions/events/charging` (device, battery, Wi-Fi, upload progress) |
| Device events | `POST {DATA}/api/v1/devices/{id}/events/bulk` |
| Firmware + config | `GET {FW}/firmware?deviceId=&hwVersion=&fwVersion=` -> `{update_required, fw_url, setup}` |
| Registration | `POST {FW}/device-provisioning?deviceName=` (LoRa EUIs, keys, BLE passkey) |

Uploads run only while charging on Wi-Fi, up to 3 tries per file; a file is **deleted after a
successful upload**. The uploader reads the session ID from the folder name and
`prefix_device_horse_startedAtMs.ext` from the file name (`Unknown` = no chip), which is why our files
use that naming.
