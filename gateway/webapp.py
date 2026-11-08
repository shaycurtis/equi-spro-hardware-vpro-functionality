"""Equimetrics local web app: dashboard over the gateway's SQLite database.

    python webapp.py                       # http://localhost:8080
    python gateway.py --serial COM4 --web  # gateway + web app in one process

JSON API (kept small and storage-agnostic so it can later front AWS instead of SQLite):
    GET /api/devices                       latest reading + last-hour reception stats per device
    GET /api/readings?device=ID&hours=N    time series (bucket-averaged for long ranges)
    POST /api/ecg {device, seconds, test}  request an on-demand ECG capture (gateway --serial --web only)
    GET /api/ecg/status?id=N               progress of a capture
    GET /api/ecg/list?device=ID            recent captures (metadata)
    GET /api/ecg/capture?id=N              one capture with samples in mV
"""
from __future__ import annotations

import argparse
import array
import json
import sqlite3
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).parent
STATIC = HERE / "static"
DEFAULT_DB = HERE / "data" / "telemetry.db"
SAMPLE_PERIOD_S = 30
MAX_POINTS = 720          # above this, average into time buckets
MAX_GAP = 120             # within one gateway run + device boot, gaps up to 1 h are radio losses

EPOCH = "CAST(strftime('%s', received_at) AS INTEGER)"


def query(db: Path, sql: str, args: tuple = ()) -> list[dict]:
    if not db.exists():
        return []
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)   # read-only: the gateway owns writes
    con.row_factory = sqlite3.Row
    try:
        return [dict(r) for r in con.execute(sql, args)]
    except sqlite3.OperationalError:
        return []   # table not created yet
    finally:
        con.close()


def api_devices(db: Path) -> list[dict]:
    latest = query(db, f"""
        SELECT r.*, {EPOCH} AS t FROM readings r
        JOIN (SELECT device_id, MAX(id) AS id FROM readings GROUP BY device_id) m ON m.id = r.id
        ORDER BY r.device_id""")
    now = int(datetime.now(timezone.utc).timestamp())
    for d in latest:
        s = query(db, f"""
            SELECT COUNT(*) AS n, AVG(packets) AS adv, AVG(rssi_best) AS rssi
            FROM readings WHERE device_id = ? AND {EPOCH} >= ?""", (d["device_id"], now - 3600))[0]
        # Radio losses = gaps in the 8-bit sequence number between consecutive readings from the
        # same gateway run AND the same device boot (uptime not reset). Gaps while the gateway or
        # the device was off are not reception problems, so they are not counted.
        rows = query(db, f"""
            SELECT seq, uptime_min, session FROM readings
            WHERE device_id = ? AND {EPOCH} >= ? ORDER BY id""", (d["device_id"], now - 3600))
        missed = 0
        for a, b in zip(rows, rows[1:]):
            if a["session"] is None or a["session"] != b["session"] or b["uptime_min"] < a["uptime_min"]:
                continue
            gap = (b["seq"] - a["seq"]) % 256
            if 1 < gap <= MAX_GAP:
                missed += gap - 1
        expected = s["n"] + missed
        d["age_s"] = now - d["t"]
        d["online"] = d["age_s"] < 3 * SAMPLE_PERIOD_S
        d["hour"] = {
            "readings": s["n"],
            "expected": expected,
            "adverts_per_burst": round(s["adv"], 1) if s["adv"] is not None else None,
            "rssi": round(s["rssi"]) if s["rssi"] is not None else None,
        }
    return latest


def api_readings(db: Path, device: str, hours: float) -> dict:
    now = int(datetime.now(timezone.utc).timestamp())
    since = now - int(hours * 3600)
    bucket = max(1, int(hours * 3600 / MAX_POINTS))
    if bucket <= SAMPLE_PERIOD_S:
        rows = query(db, f"""
            SELECT {EPOCH} AS t, battery_soc AS soc, battery_mv AS mv, temp_c AS temp, humidity AS rh,
                   steps, rssi_best AS rssi, packets, usb, charging, charge_full
            FROM readings WHERE device_id = ? AND {EPOCH} >= ? ORDER BY id""", (device, since))
    else:
        rows = query(db, f"""
            SELECT MIN({EPOCH}) AS t, AVG(battery_soc) AS soc, AVG(battery_mv) AS mv, AVG(temp_c) AS temp,
                   AVG(humidity) AS rh, MAX(steps) AS steps, AVG(rssi_best) AS rssi, AVG(packets) AS packets,
                   MAX(usb) AS usb, MAX(charging) AS charging, MAX(charge_full) AS charge_full
            FROM readings WHERE device_id = ? AND {EPOCH} >= ?
            GROUP BY {EPOCH} / ? ORDER BY t""", (device, since, bucket))
    # Steps per interval: the counter restarts at each power-on, so treat a drop as a reset.
    prev = None
    for r in rows:
        s = r["steps"]
        if s is None:
            r["steps_delta"] = None
            continue
        if prev is None:
            r["steps_delta"] = 0
        elif s < prev:
            r["steps_delta"] = s          # counter restarted (device powered on again)
        else:
            r["steps_delta"] = s - prev
        prev = s
    return {"device": device, "hours": hours, "bucket_s": bucket if bucket > SAMPLE_PERIOD_S else SAMPLE_PERIOD_S,
            "rows": rows}


def api_ecg_list(db: Path, device: str) -> list[dict]:
    return query(db, """
        SELECT id, device_id, requested_at, completed_at, seconds, test_signal, status, detail,
               sample_rate, n_samples, missing, overflows
        FROM ecg_captures WHERE device_id = ? ORDER BY id DESC LIMIT 20""", (device,))


def api_ecg_capture(db: Path, cap_id: int) -> dict | None:
    rows = query(db, "SELECT * FROM ecg_captures WHERE id = ?", (cap_id,))
    if not rows:
        return None
    r = rows[0]
    blob = r.pop("samples")
    uv = r.get("uv_per_lsb") or 0
    if blob and uv:
        vals = array.array("h")
        vals.frombytes(blob)
        r["mv"] = [round(v * uv / 1000.0, 4) for v in vals]
    else:
        r["mv"] = []
    return r


def make_handler(db: Path, ecg=None):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):   # keep the gateway console readable
            pass

        def send(self, code: int, body: bytes, ctype: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def send_json(self, code: int, obj) -> None:
            self.send(code, json.dumps(obj).encode(), "application/json")

        def do_POST(self):
            url = urlparse(self.path)
            try:
                if url.path != "/api/ecg":
                    return self.send(404, b"not found", "text/plain")
                if ecg is None:
                    return self.send_json(503, {"error": "ECG capture needs the gateway: "
                                                         "python gateway.py --serial COMx --web"})
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0) or b"{}")
                try:
                    cap = ecg.request(str(body["device"]), int(body.get("seconds", 10)), bool(body.get("test", False)))
                except RuntimeError as e:
                    return self.send_json(409, {"error": str(e)})
                self.send_json(200, cap)
            except Exception as e:
                self.send_json(500, {"error": str(e)})

        def do_GET(self):
            url = urlparse(self.path)
            q = parse_qs(url.query)
            try:
                if url.path in ("/", "/index.html"):
                    self.send(200, (STATIC / "index.html").read_bytes(), "text/html; charset=utf-8")
                elif url.path == "/api/devices":
                    self.send(200, json.dumps(api_devices(db)).encode(), "application/json")
                elif url.path == "/api/readings":
                    dev = q.get("device", [""])[0]
                    hours = min(24 * 30, max(0.1, float(q.get("hours", ["6"])[0])))
                    self.send(200, json.dumps(api_readings(db, dev, hours)).encode(), "application/json")
                elif url.path == "/api/ecg/status":
                    cid = int(q.get("id", ["0"])[0])
                    st = ecg.status(cid) if ecg else None
                    if st is None:
                        rows = query(db, "SELECT id, device_id, status, detail, seconds, test_signal, n_samples "
                                         "AS received FROM ecg_captures WHERE id = ?", (cid,))
                        st = rows[0] if rows else None
                    self.send_json(200 if st else 404, st or {"error": "unknown capture"})
                elif url.path == "/api/ecg/list":
                    self.send_json(200, api_ecg_list(db, q.get("device", [""])[0]))
                elif url.path == "/api/ecg/capture":
                    cap = api_ecg_capture(db, int(q.get("id", ["0"])[0]))
                    self.send_json(200 if cap else 404, cap or {"error": "unknown capture"})
                else:
                    self.send(404, b"not found", "text/plain")
            except Exception as e:   # never take the gateway down because of a bad request
                self.send(500, str(e).encode(), "text/plain")

    return Handler


def serve(db: Path = DEFAULT_DB, port: int = 8080, host: str = "127.0.0.1", background: bool = False, ecg=None):
    httpd = ThreadingHTTPServer((host, port), make_handler(db, ecg))
    print(f"Web app: http://localhost:{port}")
    if background:
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        return httpd
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Equimetrics local web app")
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    serve(a.db, a.port)
