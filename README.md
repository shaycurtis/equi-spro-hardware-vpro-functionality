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
| LEDs | ✅ D1 short blue blink every 5 s while on; D2 shows charge state (red/green/orange), both LEDs when off on USB; both blink green while waiting for an RFID tag |
| Wi-Fi provisioning | ✅ Espressif `network_provisioning` (SoftAP, security 1), **ESP SoftAP Provisioning** app; per-device setup code; up to 5 saved networks in NVS |
| Sensors (30 s) | ✅ Battery (MAX17048), temp/RH (SHTC3), steps (LSM6DSOX pedometer), RTC (PCF85063A) |
| GPS | ✅ Parked in backup mode at boot (`$PAIR650,0`, verified) |
| BLE telemetry | ✅ Advert v5 every 30 s (2 s burst, +15 dBm): battery (1/256 %), temp/RH, steps, heart rate, 1-byte session tag; **encrypted** identity scan response (AES-CCM, fleet key). Designed for listen-only commercial gateways |
| PC gateway + dashboard | ✅ SQLite, devices, sessions, charts, ECG panel |
| On-demand ECG | ✅ MAX30001 512 sps over BLE GATT, verified with real electrodes (74 bpm, ~15 µV noise) |
| Sessions + RFID HorseID | ✅ Session (Unix time) starts at the tag scan, or scan-button override (chip "unknown"); JY-LD6900, 9600 ASCII; per-session dashboard view |
| Power optimisation (2e) | 🟡 Light sleep works, but the overnight run measured **~12-14 mA** (≈4.5-5 days on 1500 mAh) against a 3-6 mA estimate. Parked until the PPK2 is found (see below); SD logging adds to it, overnight test running |
| Heart rate | ✅ MAX30001 hardware R-to-R detector, median over 30 s, in advert v4 (verified 76-77 bpm on battery) |
| Respiration | ⏳ Planned (1 spare advert byte reserved) |
| SD card ECG logging (phase 3) | ✅ `/sessions/<session>/ecg_<device>_<chip>_<session>.edf`, 512/256 Hz per `config.json`, interrupt-driven (light sleep kept), 0 overflows |
| SD card IMU logging | ✅ `imu_…edf`: accel ±16 g + gyro ±2000 dps at 104/416 Hz, LIS2MDL mag at 104 Hz (sensor hub), FIFO + INT1, 0 overruns |
| Wi-Fi upload | ⏳ Not started |
| Range test (50 m barn target) | ⏳ Not done yet (bench: ~18/20 adverts at -37 dBm) |

## Device behaviour (user view)

0. **Locked (no Wi-Fi saved):** power on and **both LEDs are steady white**: provisioning mode (below).
1. **Power on:** hold the power button ~1.5 s. Green double flash, then **both LEDs blink green**: scan the horse's tag.
2. **Tag read** starts the session and its SD recording. No tag? Press the **scan button** during the wait to start it anyway (chip "unknown"). After a 2 min timeout with neither, no session starts (press the scan button to scan again). Then: D1 **steady blue** until a trusted gateway has received the session (encrypted identity), then a short blink every 5 s, **green while a gateway is hearing it, blue if not**, D2 off (or charge colour on USB). The **scan button (SW1)** rescans; a different horse starts a new session.
3. **Running:** heart rate monitored continuously; every 30 s, sample sensors and send a BLE advert burst. The chip light-sleeps in between.
4. **Plugging in USB** while running on battery ends the session automatically (after 2 s): orange flashes, then charge-only (D2 red/green). Booting while already on USB keeps running (bench use).
5. **Power off:** hold the power button 2 s (orange, then 3 orange flashes). On battery the board is fully unpowered; on USB it goes to charge-only (**both LEDs** red = charging, green = full, orange = fault).
6. **Add a Wi-Fi network:** hold **both buttons** for 3 s, from off (power-on with both held) or while running: 3 white flashes, then steady white.

### SD card recording

- `/sdcard/config.json` (created with defaults if missing; later edited from the web app):
  `{"version":1,"ecg":{"log":true,"rate_hz":512},"imu":{"log":false,"rate_hz":104}}`
  (ECG 256/512 Hz; IMU 104/416 Hz for accel + gyro, magnetometer always 104 Hz).
- Each session writes `/sessions/<session>/<type>_<device id>_<chip number>_<session>.edf`, e.g.
  `/sessions/1794188521/ecg_15B0B4_999012345678436_1794188521.edf`. ECG and IMU are separate files.
- ECG EDF: 1 s records, int16, 0.381 uV/LSB, header patient = `HorseID <chip>`, recording =
  `Device EQ-<id> Session <session>`, start = session start (UTC). Written every 30 s with the record
  count updated and the file synced, so a battery cut loses at most 30 s. A different horse switches
  to a new session file at the exact sample.
- The MAX30001 interrupt (INTB, open drain to 1.8 V) wakes the chip every 16 samples; light sleep
  continues in between. Measured: exactly 512.0 sps, 32 reads/s, 0 overflows. BLE ECG requests tap
  the same stream (512 Hz only).
- IMU EDF: AccX/Y/Z (g, 0.488 mg/LSB), GyrX/Y/Z (dps, 70 mdps/LSB) at the IMU rate, MagX/Y/Z (uT,
  0.15 uT/LSB) at 104 Hz, raw sensor axes. The record duration is the IMU's real one from its
  oscillator trim (e.g. 0.99552 s at "416 Hz" = 417.87 Hz), so it stays aligned with the ECG's
  crystal timeline. Both files start at the session start (each stream is padded back to it).
- The magnetometer includes the board's hard-iron offset (~(-124, -13, 19) uT on this unit, from the
  buzzer / motor / battery); calibrate it out in analysis (rotate through all orientations, subtract
  the centre). Measured: 25137 accel + gyro and 6285 mag samples per minute, 0 overruns.
- Check a file: `python tools/edf_info.py F:/sessions/<session>/ecg_…edf` (or open in EDFbrowser).
- Card: Transcend industrial 32 GB, 815 KB/s write over SPI3 at 20 MHz (ECG needs ~1 KB/s).

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
- **ECG GATT service:** `equi-sensordevice-vpro/components/telemetry/ecg_service.h` (start/stop/set-time, 112-sample notifications).
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

**In progress (night of 2026-11-09):** overnight run on battery with SD logging on (ECG 512 Hz +
IMU 416 Hz) and the PC gateway receiving. In the morning: check the gateway's battery curve (drain
with logging on vs the ~12-14 mA baseline), each EDF file with `tools/edf_info.py` (record counts vs
file size, no gaps), and the readings / losses in the dashboard.

1. **Wi-Fi while charging:** use the provisioned networks for time sync, a backend check-in, session
   file upload, OTA (signature-checked, with rollback) and the trusted-gateway list.
2. **Web Bluetooth config page:** edit `config.json` settings and view the saved networks from a
   browser (Chrome on Android / desktop; Bluefy on iOS).
3. **Power:** the parked PPK2 drain test (SD card is the top suspect for the extra ~8 mA), plus the
   cost of logging (ECG interrupt every 31 ms, IMU FIFO reads ~30/s at 416 Hz); then shorten the 2 s
   burst and tune TX power after the range test.
4. **Range test** on a horse in the barn (target 50 m): gateway `--summary 60`, walk the device out.
5. **Respiration** (MAX30001 BioZ or ECG-derived), and checking R-to-R detection on a horse ECG
   (large T waves may need WNDW/PTSF tuning).
6. Tidy-ups: quieten the per-burst scanner log; magnetometer hard-iron calibration in analysis.
7. Production items: SIG company ID, off-the-shelf listen-only gateway (active scan + scan
   responses), per-device keys, AWS backend, test-lab check that +15 dBm stays within the SPRO
   certification.
