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

Requirements: Python 3.10+, `pip install -r requirements.txt` (bleak, pyserial). The
dashboard loads Chart.js from cdn.jsdelivr.net, so the browser needs internet access.

Dashboard: device cards (battery, temp/RH, steps, heart rate, charge, HorseID, **battery drain
estimate** in mA and runtime), a **Sessions** strip
(click one to see only that session; 1h/6h/24h/7d show all data by time), charts, and an
**ECG on demand** panel (needs `--serial`; the device is contacted at its next advert, up to 30 s).

Files:
- `equi_protocol.py`: advert decoder (telemetry v1-v4 + identity), mirrors `components/telemetry/include/telemetry.h`.
- `gateway.py`: receiver loop, de-duplication, SQLite, sessions, automatic device time sync.
- `ecg_manager.py`: on-demand ECG via the receiver (request, reassembly, storage).
- `webapp.py`: JSON API + `static/index.html`.

SQLite tables (`data/telemetry.db`, git-ignored): `readings` (one row per 30 s advert, with
`device_session`, `horse_id`, `heart_rate`), `device_sessions` (session ID, HorseID, start, last seen),
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
