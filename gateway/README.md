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

Files:
- `equi_protocol.py`: advert decoder, mirrors `components/telemetry/include/telemetry.h`.
- `gateway.py`: receiver loop, de-duplication, SQLite.
- `webapp.py`: JSON API (`/api/devices`, `/api/readings`) + `static/index.html`.

The API is deliberately small and storage-agnostic, so the same dashboard can later sit
in front of AWS (e.g. gateway publishes `Telemetry.to_record()` to MQTT / IoT Core).
