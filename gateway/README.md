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

Requirements: Python 3.10+, `pip install -r requirements.txt` (bleak, pyserial). The
dashboard loads Chart.js from cdn.jsdelivr.net, so the browser needs internet access.

Dashboard: device cards (battery, temp/RH, steps, charge, RSSI, HorseID), a **Sessions** strip
(click one to see only that session; 1h/6h/24h/7d show all data by time), charts, and an
**ECG on demand** panel (needs `--serial`; the device is contacted at its next advert, up to 30 s).

Files:
- `equi_protocol.py`: advert decoder (telemetry v1/v2 + identity), mirrors `components/telemetry/include/telemetry.h`.
- `gateway.py`: receiver loop, de-duplication, SQLite, sessions, automatic device time sync.
- `ecg_manager.py`: on-demand ECG via the receiver (request, reassembly, storage).
- `webapp.py`: JSON API + `static/index.html`.

SQLite tables (`data/telemetry.db`, git-ignored): `readings` (one row per 30 s advert, with
`device_session` and `horse_id`), `device_sessions` (session ID, HorseID, start, last seen),
`ecg_captures` (int16 samples, rate, uV/LSB, session, horse).

API: `GET /api/devices`, `/api/sessions?device=`, `/api/readings?device=&hours=|session=`,
`/api/ecg/list?device=[&session=]`, `/api/ecg/capture?id=`, `/api/ecg/status?id=`, `POST /api/ecg`.

The API is deliberately small and storage-agnostic, so the same dashboard can later sit
in front of AWS (e.g. gateway publishes `Telemetry.to_record()` to MQTT / IoT Core).
