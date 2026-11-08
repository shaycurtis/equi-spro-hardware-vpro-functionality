"""On-demand ECG captures via the ESP32 receiver.

Flow: request() writes "ECG,<addr>,<seconds>,<flags>,<epoch>" to the receiver, which
connects to the device the next time it advertises (within ~30 s), sets its clock,
starts the capture and forwards each notification as an EQECGD line. Samples are
reassembled by index and stored in SQLite (table ecg_captures) when the capture ends.

Packet format: see equi-sensordevice-vpro/components/telemetry/ecg_service.h.
"""
from __future__ import annotations

import array
import struct
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone

ECG_SCHEMA = """
CREATE TABLE IF NOT EXISTS ecg_captures (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id     TEXT    NOT NULL,
    requested_at  TEXT    NOT NULL,     -- ISO-8601 UTC
    completed_at  TEXT,
    seconds       INTEGER NOT NULL,
    test_signal   INTEGER NOT NULL,
    status        TEXT    NOT NULL,     -- waiting / connecting / connected / recording / done / failed
    detail        TEXT,
    sample_rate   INTEGER,
    uv_per_lsb    REAL,
    n_samples     INTEGER,
    missing       INTEGER,              -- samples expected but never received
    overflows     INTEGER,
    samples       BLOB                  -- int16 little-endian
);
CREATE INDEX IF NOT EXISTS idx_ecg_dev ON ecg_captures (device_id, id);
"""

PKT_START, PKT_DATA, PKT_END = 0xE0, 0xE1, 0xEF
FINAL = ("done", "failed")


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


@dataclass
class Capture:
    id: int
    device_id: str
    address: str
    seconds: int
    test: bool
    status: str = "waiting"
    detail: str = "waiting for the device to advertise (up to 30 s)"
    rate: int = 512
    uv_per_lsb: float = 0.0
    total: int | None = None
    overflows: int = 0
    received: int = 0
    samples: array.array = field(default_factory=lambda: array.array("h"))
    have: bytearray = field(default_factory=bytearray)

    def public(self) -> dict:
        expected = self.total if self.total is not None else self.seconds * self.rate
        return {"id": self.id, "device_id": self.device_id, "status": self.status, "detail": self.detail,
                "seconds": self.seconds, "test_signal": self.test, "received": self.received,
                "expected": expected}


class EcgManager:
    def __init__(self, db, lock: threading.Lock):
        self.db = db
        self.lock = lock
        with self.lock:
            self.db.executescript(ECG_SCHEMA)
            for col in ("device_session INTEGER", "horse_id TEXT"):
                try:
                    self.db.execute(f"ALTER TABLE ecg_captures ADD COLUMN {col}")
                except Exception:
                    pass   # already there
        self.addresses: dict[str, str] = {}   # device_id -> BLE address (from adverts)
        self.identity: dict[str, tuple] = {}  # device_id -> (session, horse_id); set by the gateway
        self.serial = None                    # set by the gateway in --serial mode
        self.active: Capture | None = None
        self.recent: dict[int, Capture] = {}

    # ---------------------------------------------------------------- request
    def request(self, device_id: str, seconds: int, test: bool) -> dict:
        if self.serial is None:
            raise RuntimeError("ECG capture needs the ESP32 receiver (run the gateway with --serial)")
        if self.active and self.active.status not in FINAL:
            raise RuntimeError(f"a capture is already in progress (device {self.active.device_id})")
        addr = self.addresses.get(device_id) or self._last_address(device_id)
        if not addr:
            raise RuntimeError(f"no BLE address known for {device_id} yet")
        seconds = max(1, min(60, int(seconds)))
        session, horse = self.identity.get(device_id, (None, None))
        with self.lock:
            cur = self.db.execute(
                "INSERT INTO ecg_captures (device_id, requested_at, seconds, test_signal, status, detail, "
                "device_session, horse_id) VALUES (?,?,?,?,?,?,?,?)",
                (device_id, now_iso(), seconds, int(test), "waiting", None, session, horse))
            self.db.commit()
            cap = Capture(cur.lastrowid, device_id, addr, seconds, test)
            self.active = cap
            self.recent[cap.id] = cap
            self.serial.write(f"ECG,{addr},{seconds},{1 if test else 0},{int(time.time())}\n".encode())
            self.serial.flush()
        print(f"ECG #{cap.id}: requested {seconds} s from {device_id} ({addr}){' [test signal]' if test else ''}")
        return cap.public()

    def sync_time(self, device_id: str) -> bool:
        """Set a device's RTC from the PC clock (connect, set time, disconnect). Skipped while a
        capture is in progress (the receiver handles one connection at a time)."""
        if self.serial is None or (self.active and self.active.status not in FINAL):
            return False
        addr = self.addresses.get(device_id)
        if not addr:
            return False
        with self.lock:
            self.serial.write(f"ECG,{addr},0,0,{int(time.time())}\n".encode())
            self.serial.flush()
        print(f"{device_id}: RTC not set on the device - sending the time")
        return True

    def _last_address(self, device_id: str) -> str | None:
        with self.lock:
            row = self.db.execute("SELECT address FROM readings WHERE device_id = ? ORDER BY id DESC LIMIT 1",
                                  (device_id,)).fetchone()
        return row[0] if row else None

    def status(self, cap_id: int) -> dict | None:
        cap = self.recent.get(cap_id)
        return cap.public() if cap else None

    # ------------------------------------------------- lines from the receiver
    def on_event(self, ev: str, addr: str, detail: str) -> None:
        cap = self.active
        if not cap or cap.address != addr or cap.status in FINAL:
            return
        if ev == "CONN":
            self._set(cap, "connecting", "connected, discovering ECG service")
        elif ev == "READY":
            self._set(cap, "connected", "subscribed, starting capture")
        elif ev == "TIMESET":
            print(f"ECG #{cap.id}: device clock set ({'ok' if detail == '0' else 'error ' + detail})")
        elif ev == "STARTED":
            self._set(cap, "recording", "recording")
        elif ev == "TIMEOUT":
            self._finish(cap, "failed", "device not seen within 45 s (is it on and in range?)")
        elif ev == "BUSY":
            self._finish(cap, "failed", "receiver busy with another session")
        elif ev == "ERR":
            self._finish(cap, "failed", f"BLE error {detail}")
        elif ev == "DISC" and cap.status not in FINAL:
            self._finish(cap, "failed", f"disconnected before the capture finished (reason {detail})")

    def on_data(self, addr: str, hexdata: str) -> None:
        cap = self.active
        if not cap or cap.address != addr or cap.status in FINAL:
            return
        try:
            p = bytes.fromhex(hexdata)
        except ValueError:
            return
        if not p:
            return
        if p[0] == PKT_START and len(p) >= 10:
            cap.rate, = struct.unpack_from("<H", p, 1)
            cap.uv_per_lsb, = struct.unpack_from("<f", p, 3)
            n = cap.seconds * cap.rate
            cap.samples = array.array("h", bytes(2 * n))
            cap.have = bytearray(n)
        elif p[0] == PKT_DATA and len(p) >= 6:
            first, = struct.unpack_from("<I", p, 1)
            n = min(p[5], (len(p) - 6) // 2)
            vals = struct.unpack_from(f"<{n}h", p, 6)
            for i, v in enumerate(vals):
                k = first + i
                if k < len(cap.samples):
                    cap.samples[k] = v
                    if not cap.have[k]:
                        cap.have[k] = 1
                        cap.received += 1
        elif p[0] == PKT_END and len(p) >= 8:
            ok = p[1] == 0
            cap.total, = struct.unpack_from("<I", p, 2)
            cap.overflows, = struct.unpack_from("<H", p, 6)
            self._finish(cap, "done" if ok else "failed",
                         "complete" if ok else "device reported an AFE error (check MAX30001 power/SPI)")

    # ----------------------------------------------------------------- helpers
    def _set(self, cap: Capture, status: str, detail: str) -> None:
        cap.status, cap.detail = status, detail
        with self.lock:
            self.db.execute("UPDATE ecg_captures SET status = ?, detail = ? WHERE id = ?", (status, detail, cap.id))
            self.db.commit()

    def _finish(self, cap: Capture, status: str, detail: str) -> None:
        total = cap.total if cap.total is not None else cap.received
        total = min(total, len(cap.samples))
        missing = sum(1 for k in range(total) if not cap.have[k]) if cap.have else total
        if status == "done" and missing:
            detail = f"complete, {missing} samples lost in transfer"
        cap.status, cap.detail = status, detail
        blob = cap.samples[:total].tobytes() if total else None
        with self.lock:
            self.db.execute(
                "UPDATE ecg_captures SET status=?, detail=?, completed_at=?, sample_rate=?, uv_per_lsb=?, "
                "n_samples=?, missing=?, overflows=?, samples=? WHERE id=?",
                (status, detail, now_iso(), cap.rate, cap.uv_per_lsb or None, total, missing, cap.overflows,
                 blob, cap.id))
            self.db.commit()
        print(f"ECG #{cap.id}: {status} - {detail} ({total} samples, {cap.overflows} FIFO overflows)")
