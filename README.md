# Equimetrics VPro firmware (SPRO PCBA)

Custom firmware for the Equimetrics SPRO sensor board (ESP32-S3-WROOM-1-N16R8), a horse-worn
chest sensor, plus a PC gateway and dashboard for development. Status as of **2026-11-08**.

> **Signing keys never leave this PC.** `**/private/` and `*.pem` are git-ignored. All
> building and signing is local.

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
| Power sequencing | ✅ Hold to power on (~1.5 s), long press to power off (2 s), charge-only deep sleep on USB |
| LEDs | ✅ D1 breathes blue while on; D2 shows charge state (red/green/orange); both blink green while waiting for an RFID tag |
| Sensors (30 s) | ✅ Battery (MAX17048), temp/RH (SHTC3), steps (LSM6DSOX pedometer), RTC (PCF85063A) |
| GPS | ✅ Parked in backup mode at boot (`$PAIR650,0`, verified) |
| BLE telemetry | ✅ Advert v2 every 30 s (2 s burst, +15 dBm), identity scan response |
| PC gateway + dashboard | ✅ SQLite, devices, sessions, charts, ECG panel |
| On-demand ECG | ✅ MAX30001 512 sps over BLE GATT, verified with real electrodes (74 bpm, ~15 µV noise) |
| Sessions + RFID HorseID | ✅ Session = Unix time at boot; tag scanned at boot (JY-LD6900, 9600 ASCII); per-session dashboard view |
| Power optimisation (2e) | ⏳ **Next**: light sleep, current measurement, burst/TX-power tuning |
| Heart rate / respiration in adverts | ⏳ Planned: MAX30001 hardware R-to-R detector, 3 spare advert bytes reserved |
| SD card EDF logging + Wi-Fi upload (phase 3) | ⏳ Not started |
| Range test (50 m barn target) | ⏳ Not done yet (bench: ~18/20 adverts at -37 dBm) |

## Device behaviour (user view)

1. **Power on:** hold the power button ~1.5 s. Green double flash, then **both LEDs blink green**: scan the horse's tag.
2. **Tag read** (or 2 min timeout): D1 breathes blue, D2 off (or charge colour on USB). The **scan button (SW1)** rescans; a different horse starts a new session.
3. **Running:** every 30 s, sample sensors and send a BLE advert burst.
4. **Power off:** hold the power button 2 s (orange, then 3 orange flashes). On battery the board is fully unpowered; on USB it goes to charge-only (D2 red = charging, green = full).

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

- **Telemetry advert v2 + identity scan response:** `equi-sensordevice-vpro/components/telemetry/include/telemetry.h`
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
- **RTC oscillator-stop flag** was seen once after the RFID reader switched on (coin cell is fitted).
  Firmware tolerates it (plausible date still used) and the gateway re-syncs time. Measure the coin cell
  if it recurs.
- **Gateway DB**: readings are not unique by (seq, uptime); de-dup is in memory per burst. Session
  losses are only counted within one gateway run + one device session.

## Next steps

1. **Phase 2e, power:** automatic light sleep between events (no 32 kHz crystal for the ESP32, so
   measure what BLE sleep achieves), measure current on battery, shorten the 2 s burst (~18/20 adverts
   arrive), tune TX power after the range test, decide breathing LED vs short pulse.
2. **Range test** on a horse in the barn (target 50 m): gateway `--summary 60`, walk the device out.
3. **Heart rate (and later respiration)** in the advert via the MAX30001 R-to-R detector.
4. **Phase 3:** 512 Hz ECG + 416 Hz IMU to SD in EDF; Wi-Fi upload while charging.
5. Production items: SIG company ID, commercial gateway with BLE connection support (for ECG), AWS backend.
