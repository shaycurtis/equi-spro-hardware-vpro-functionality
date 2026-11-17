# Equimetrics PC gateway + dashboard (development)

Receives VPro telemetry adverts, stores them in SQLite (`data/telemetry.db`) and serves a
local dashboard.

```
python gateway.py --serial COM4 --web
```

Then open http://localhost:8080.

| Option | Meaning |
|---|---|
| `--serial COMx` | Read from the ESP32 receiver dev kit (`../gateway-receiver`). Recommended. Without it, the PC's own Bluetooth is used. |
| `--web [PORT]` | Also serve the dashboard (default 8080). `python webapp.py` serves it on its own. |
| `--summary N` | Print reception stats every N s (adverts per burst, RSSI, missed readings). |
| `--verbose` | Print every advert, not just one line per reading. |
| `--duration N` | Stop after N seconds. |
| `--battery-mah N` | Battery capacity for the drain/runtime estimate (default 1500). |
| `--mqtt [ENDPOINT]` | Also publish each burst to AWS IoT Core (postBleData Lambda; see `cloud/README.md`). Certificates and `endpoint.txt` (the default endpoint) in `<repo>/private/iot/`. |
| `--mqtt-dry-run` | Build the MQTT messages without AWS; written to `data/mqtt_out/`. |
| `--mqtt-thing NAME` | IoT thing name = MQTT client ID, gatewayId and topic (default `equi-pc-gateway-dev`). |
| `--mqtt-certs DIR`, `--mqtt-dump DIR` | Other certificate folder; save every message as JSON. |

Requirements: Python 3.10+, `pip install -r requirements.txt` (bleak, pyserial). The
dashboard loads Chart.js from cdn.jsdelivr.net, so the browser needs internet access.

Dashboard: device cards (battery, temp/RH, steps, heart rate, charge, HorseID, **battery drain
estimate** in mA and runtime), a **Sessions** strip
(click one to see only that session; 1h/6h/24h/7d show all data by time), charts, and an
**ECG on demand** panel (needs `--serial`; the device is contacted at its next advert, up to 60 s).

Files:
- `equi_protocol.py`: advert decoder (telemetry v1-v6 + identity v1-v3), mirrors `components/telemetry/include/telemetry.h`.
- `gateway.py`: receiver loop, de-duplication, SQLite, sessions, automatic device time sync.
- `ecg_manager.py`: on-demand ECG via the receiver (request, reassembly, storage).
- `webapp.py`: JSON API + `static/index.html`.
- `mqtt_publisher.py`: `--mqtt`: one message per device per burst (all advert copies + the latest
  identity), topic `equimetrics/ble/<thing>/adv`, the postBleData input format. A second gateway's
  copy of the same burst, and an advert whose scan response arrives in a later message, are
  handled in the Lambda (`cloud/README.md`).

SQLite tables (`data/telemetry.db`, git-ignored): `readings` (one row per 60 s advert, with
`device_session`, `horse_id`, `heart_rate`, `position`, `positions_seen`, `horse_temp` (DS18B20 probe, °C)), `device_sessions` (session ID, HorseID, start, last seen),
`ecg_captures` (int16 samples, rate, uV/LSB, session, horse).

API: `GET /api/devices`, `/api/sessions?device=`, `/api/readings?device=&hours=|session=`,
`/api/ecg/list?device=[&session=]`, `/api/ecg/capture?id=`, `/api/ecg/status?id=`, `POST /api/ecg`.

The API is deliberately small and storage-agnostic, so the same dashboard can later sit
in front of AWS (e.g. gateway publishes `Telemetry.to_record()` to MQTT / IoT Core).

## Gateway pairing (green heartbeat on the device)

Adverts with flag bit 6 (`no_gateway`) mean no trusted gateway has heard the device recently. The
gateway then connects through the receiver and sets the device clock (`ECG,<addr>,0,0,<epoch>`),
at most every 2 minutes per device. Any successful command write makes the device trust this
gateway, and its scan requests then turn the device's 5 s heartbeat green.

## Advert v5 and the encrypted identity

v5 adverts carry only a 1-byte session tag. The session ID and HorseID travel in the scan response,
encrypted with AES-128-CCM under the fleet key (`<repo>/private/fleet_key.bin`, or `--fleet-key`).
Without the key the gateway still stores readings, but can't attach them to a session or horse.
Readings heard before the identity are stored with `session_tag` and no `device_session`, and are
attached when the identity arrives (within 24 h).

Since 2026-11-09 a device session starts at the tag scan (or the scan-button override, HorseID
"Unknown"), not at boot. Until then the device advertises with session tag 0 and no identity, so
those readings stay without a session; that is expected (the device is on but not on a horse yet).

Session IDs are **Unix time in milliseconds** since 2026-11-09 (as the VPro platform), carried in
identity **v3** (0x83, 8-byte session, random IDs flagged by bit 52 so the dashboard's JavaScript
keeps them exact). Older firmware sent seconds (identity v2, bit 31 = random); both are read, and
`session_start()` / `session_is_random()` in `equi_protocol.py` handle either.

The device also runs a second, connectable advert for the Equimetrics app (name only, no
manufacturer data); the gateway ignores it.
