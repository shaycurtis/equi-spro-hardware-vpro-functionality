"""Equimetrics local web app: dashboard over the gateway's SQLite database.

    python webapp.py                       # http://localhost:8080
    python gateway.py --serial COM4 --web  # gateway + web app in one process

JSON API (kept small and storage-agnostic so it can later front AWS instead of SQLite):
    GET /api/devices                       latest reading + last-hour reception stats per device
    GET /api/sessions?device=ID            device sessions (boot / horse scan), newest first
    GET /api/readings?device=ID&hours=N    time series (bucket-averaged for long ranges)
    GET /api/readings?device=ID&session=S  one session's time series
    POST /api/ecg {device, seconds, test}  request an on-demand ECG capture (gateway --serial --web only)
    GET /api/ecg/status?id=N               progress of a capture
    GET /api/ecg/list?device=ID[&session=S] recent captures (metadata)
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
BATTERY_MAH = 1500.0      # overridden by serve(battery_mah=...)
DRAIN_WINDOW_S = 6 * 3600 # fit the battery trend over at most the last 6 h of the session
DRAIN_MIN_SPAN_S = 1800   # need 30 min on battery before reporting a drain estimate


def drain_estimate(db: Path, device: str, session) -> dict:
    """Battery drain from the MAX17048 state of charge: least-squares slope of SoC vs time over
    the session's battery-only readings (USB connected = charging, not counted)."""
    if session is None:
        return {"status": "no_session"}
    now = int(datetime.now(timezone.utc).timestamp())
    rows = query(db, f"""
        SELECT {EPOCH} AS t, battery_soc AS soc, usb FROM readings
        WHERE device_id = ? AND device_session = ? AND {EPOCH} >= ? AND battery_soc IS NOT NULL
        ORDER BY id""", (device, session, now - DRAIN_WINDOW_S))
    if rows and rows[-1]["usb"]:
        return {"status": "usb"}
    # Only the most recent unbroken stretch on battery.
    run = []
    for r in reversed(rows):
        if r["usb"]:
            break
        run.append(r)
    run.reverse()
    span = run[-1]["t"] - run[0]["t"] if len(run) > 1 else 0
    if span < DRAIN_MIN_SPAN_S or len(run) < 10:
        return {"status": "measuring", "span_min": round(span / 60), "need_min": DRAIN_MIN_SPAN_S // 60}
    n = len(run)
    mt = sum(r["t"] for r in run) / n
    ms = sum(r["soc"] for r in run) / n
    sxx = sum((r["t"] - mt) ** 2 for r in run)
    sxy = sum((r["t"] - mt) * (r["soc"] - ms) for r in run)
    pct_per_h = -(sxy / sxx) * 3600 if sxx else 0.0      # positive = discharging
    soc_now = run[-1]["soc"]
    out = {"status": "ok", "span_min": round(span / 60), "points": n, "pct_per_h": round(pct_per_h, 3),
           "soc": soc_now, "battery_mah": BATTERY_MAH}
    if pct_per_h <= 0.01:
        out["status"] = "flat"   # within gauge noise: drain too small to resolve yet
        return out
    out["ma"] = round(pct_per_h / 100.0 * BATTERY_MAH, 2)
    out["hours_left"] = round(soc_now / pct_per_h, 1)
    return out

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
            SELECT seq, uptime_min, session, device_session FROM readings
            WHERE device_id = ? AND {EPOCH} >= ? ORDER BY id""", (d["device_id"], now - 3600))
        missed = 0
        for a, b in zip(rows, rows[1:]):
            if a["session"] is None or a["session"] != b["session"]:
                continue   # gateway restarted in between
            if a["device_session"] is not None or b["device_session"] is not None:
                if a["device_session"] != b["device_session"]:
                    continue   # device rebooted / new horse session
            elif b["uptime_min"] < a["uptime_min"]:
                continue       # v1 firmware: uptime reset = reboot
            gap = (b["seq"] - a["seq"]) % 256
            if 1 < gap <= MAX_GAP:
                missed += gap - 1
        expected = s["n"] + missed
        d["age_s"] = now - d["t"]
        ds = d.get("device_session")
        sess = query(db, "SELECT started_at, horse_id, readings FROM device_sessions WHERE device_id = ? AND session_id = ?",
                     (d["device_id"], ds)) if ds is not None else []
        d["device_session_info"] = sess[0] if sess else None
        d["drain"] = drain_estimate(db, d["device_id"], ds)
        d["online"] = d["age_s"] < 3 * SAMPLE_PERIOD_S
        d["hour"] = {
            "readings": s["n"],
            "expected": expected,
            "adverts_per_burst": round(s["adv"], 1) if s["adv"] is not None else None,
            "rssi": round(s["rssi"]) if s["rssi"] is not None else None,
        }
    return latest


def api_sessions(db: Path, device: str) -> list[dict]:
    now = int(datetime.now(timezone.utc).timestamp())
    rows = query(db, f"""
        SELECT s.session_id, s.horse_id, s.started_at, s.first_seen, s.last_seen, s.readings,
               (SELECT COUNT(*) FROM ecg_captures e WHERE e.device_id = s.device_id
                  AND e.device_session = s.session_id AND e.status = 'done') AS ecg_count
        FROM device_sessions s WHERE s.device_id = ? ORDER BY s.first_seen DESC LIMIT 100""", (device,))
    if not rows:   # older databases without ecg_captures.device_session
        rows = query(db, """
            SELECT session_id, horse_id, started_at, first_seen, last_seen, readings, 0 AS ecg_count
            FROM device_sessions WHERE device_id = ? ORDER BY first_seen DESC LIMIT 100""", (device,))
    for i, r in enumerate(rows):
        last = int(datetime.fromisoformat(r["last_seen"]).timestamp())
        start = r["started_at"] or r["first_seen"]
        r["start_t"] = int(datetime.fromisoformat(start).timestamp())
        r["last_t"] = last
        r["live"] = i == 0 and now - last < 3 * SAMPLE_PERIOD_S
        r["random_id"] = bool(r["session_id"] & (1 << 31))
    return rows


def api_readings(db: Path, device: str, hours: float, session: int | None = None) -> dict:
    now = int(datetime.now(timezone.utc).timestamp())
    if session is None:
        where, args = f"device_id = ? AND {EPOCH} >= ?", (device, now - int(hours * 3600))
        span = int(hours * 3600)
    else:
        where, args = "device_id = ? AND device_session = ?", (device, session)
        rng = query(db, f"SELECT MIN({EPOCH}) AS a, MAX({EPOCH}) AS b FROM readings WHERE {where}", args)
        a, b = (rng[0]["a"], rng[0]["b"]) if rng and rng[0]["a"] is not None else (now, now)
        span = max(SAMPLE_PERIOD_S, b - a)
    bucket = max(1, int(span / MAX_POINTS))
    if bucket <= SAMPLE_PERIOD_S:
        rows = query(db, f"""
            SELECT {EPOCH} AS t, battery_soc AS soc, battery_mv AS mv, temp_c AS temp, humidity AS rh,
                   steps, rssi_best AS rssi, packets, usb, charging, charge_full
            FROM readings WHERE {where} ORDER BY id""", args)
    else:
        rows = query(db, f"""
            SELECT MIN({EPOCH}) AS t, AVG(battery_soc) AS soc, AVG(battery_mv) AS mv, AVG(temp_c) AS temp,
                   AVG(humidity) AS rh, MAX(steps) AS steps, AVG(rssi_best) AS rssi, AVG(packets) AS packets,
                   MAX(usb) AS usb, MAX(charging) AS charging, MAX(charge_full) AS charge_full
            FROM readings WHERE {where}
            GROUP BY {EPOCH} / ? ORDER BY t""", args + (bucket,))
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
    return {"device": device, "hours": hours, "session": session,
            "bucket_s": bucket if bucket > SAMPLE_PERIOD_S else SAMPLE_PERIOD_S, "rows": rows}


def api_ecg_list(db: Path, device: str, session: int | None = None) -> list[dict]:
    cols = """id, device_id, requested_at, completed_at, seconds, test_signal, status, detail,
              sample_rate, n_samples, missing, overflows"""
    if session is not None:
        return query(db, f"""SELECT {cols} FROM ecg_captures WHERE device_id = ? AND device_session = ?
                             ORDER BY id DESC LIMIT 50""", (device, session))
    return query(db, f"SELECT {cols} FROM ecg_captures WHERE device_id = ? ORDER BY id DESC LIMIT 20", (device,))


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
                elif url.path == "/api/sessions":
                    self.send_json(200, api_sessions(db, q.get("device", [""])[0]))
                elif url.path == "/api/readings":
                    dev = q.get("device", [""])[0]
                    hours = min(24 * 30, max(0.1, float(q.get("hours", ["6"])[0])))
                    sess = q.get("session", [None])[0]
                    self.send_json(200, api_readings(db, dev, hours, int(sess) if sess else None))
                elif url.path == "/api/ecg/status":
                    cid = int(q.get("id", ["0"])[0])
                    st = ecg.status(cid) if ecg else None
                    if st is None:
                        rows = query(db, "SELECT id, device_id, status, detail, seconds, test_signal, n_samples "
                                         "AS received FROM ecg_captures WHERE id = ?", (cid,))
                        st = rows[0] if rows else None
                    self.send_json(200 if st else 404, st or {"error": "unknown capture"})
                elif url.path == "/api/ecg/list":
                    sess = q.get("session", [None])[0]
                    self.send_json(200, api_ecg_list(db, q.get("device", [""])[0], int(sess) if sess else None))
                elif url.path == "/api/ecg/capture":
                    cap = api_ecg_capture(db, int(q.get("id", ["0"])[0]))
                    self.send_json(200 if cap else 404, cap or {"error": "unknown capture"})
                else:
                    self.send(404, b"not found", "text/plain")
            except Exception as e:   # never take the gateway down because of a bad request
                self.send(500, str(e).encode(), "text/plain")

    return Handler


def serve(db: Path = DEFAULT_DB, port: int = 8080, host: str = "127.0.0.1", background: bool = False, ecg=None,
          battery_mah: float = 1500.0):
    global BATTERY_MAH
    BATTERY_MAH = float(battery_mah)
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
