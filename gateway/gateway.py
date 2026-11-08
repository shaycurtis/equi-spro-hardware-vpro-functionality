"""Equimetrics PC BLE gateway (development).

Scans for VPro telemetry adverts, de-duplicates each 2 s burst (one reading per
device+seq), prints it, and stores it in SQLite. Also tracks reception quality for
range testing: adverts heard per burst, best RSSI, and missed bursts (seq gaps).

    python gateway.py --serial COM4 --web   # ESP32 receiver + dashboard at http://localhost:8080
    python gateway.py --serial COM4   # via the ESP32 receiver dev kit (recommended)
    python gateway.py                 # via the PC's own Bluetooth
    python gateway.py --verbose       # also print every individual advert
"""
from __future__ import annotations

import argparse
import asyncio
import signal
import sqlite3
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

from equi_protocol import COMPANY_ID, Telemetry, decode

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at   TEXT    NOT NULL,     -- ISO-8601 UTC, first advert of the burst
    device_id     TEXT    NOT NULL,
    seq           INTEGER NOT NULL,
    uptime_min    INTEGER NOT NULL,
    battery_soc   INTEGER,
    battery_mv    INTEGER,
    temp_c        REAL,
    humidity      REAL,
    steps         INTEGER,
    usb           INTEGER,
    charging      INTEGER,
    charge_full   INTEGER,
    charger_fault INTEGER,
    rtc_valid     INTEGER,
    rssi_best     INTEGER,
    packets       INTEGER,              -- adverts heard in this burst (range indicator)
    address       TEXT,
    session       INTEGER,              -- gateway run (start time); loss stats only compare within one
    UNIQUE (device_id, seq, uptime_min)
);
CREATE INDEX IF NOT EXISTS idx_readings_dev_time ON readings (device_id, received_at);
"""


@dataclass
class DeviceStats:
    last_seq: int | None = None
    bursts: int = 0
    missed: int = 0
    packets: int = 0
    rssi: list[int] = field(default_factory=list)


class Gateway:
    def __init__(self, db_path: Path, verbose: bool):
        db_path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(db_path, check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=WAL")   # web app reads while we write
        self.db.executescript(SCHEMA)
        try:   # databases created before the session column existed
            self.db.execute("ALTER TABLE readings ADD COLUMN session INTEGER")
        except sqlite3.OperationalError:
            pass
        self.session = int(time.time())
        self.verbose = verbose
        self.current: dict[str, tuple[int, int, int]] = {}   # device -> (seq, uptime, row id)
        self.stats: dict[str, DeviceStats] = {}

    def on_advert(self, device: BLEDevice, adv: AdvertisementData) -> None:
        """bleak callback (PC Bluetooth)."""
        payload = adv.manufacturer_data.get(COMPANY_ID)
        if payload:
            self.handle(device.address, adv.rssi, bytes(payload))

    def on_serial_line(self, line: str) -> None:
        """Line from the ESP32 receiver (gateway-receiver firmware)."""
        parts = line.strip().split(",")
        if parts[0] == "EQADV" and len(parts) == 4:
            try:
                self.handle(parts[1], int(parts[2]), bytes.fromhex(parts[3]))
            except ValueError:
                pass
        elif parts[0] == "EQRX":
            print("receiver ready")
        elif parts[0] == "EQHB" and self.verbose:
            print(f"  receiver heartbeat: up {parts[1]} s, {parts[2]} adverts forwarded")

    def handle(self, address: str, rssi: int, payload: bytes) -> None:
        t = decode(payload)
        if t is None:
            return
        if self.verbose:
            print(f"  adv {t.device_id} seq={t.seq} rssi={rssi}")

        key = (t.seq, t.uptime_min)
        cur = self.current.get(t.device_id)
        st = self.stats.setdefault(t.device_id, DeviceStats())
        if cur and (cur[0], cur[1]) == key:
            # Another advert from the same burst: update reception stats only.
            st.packets += 1
            st.rssi.append(rssi)
            self.db.execute(
                "UPDATE readings SET packets = packets + 1, rssi_best = MAX(rssi_best, ?) WHERE id = ?",
                (rssi, cur[2]),
            )
            self.db.commit()
            return

        # New burst = new reading.
        if st.last_seq is not None:
            gap = (t.seq - st.last_seq) % 256
            if gap > 1 and t.uptime_min >= (cur[1] if cur else 0):
                st.missed += gap - 1
        st.last_seq = t.seq
        st.bursts += 1
        st.packets += 1
        st.rssi.append(rssi)

        row_id = self.store(t, rssi, address)
        self.current[t.device_id] = (t.seq, t.uptime_min, row_id)
        self.print_reading(t, rssi)

    def store(self, t: Telemetry, rssi: int, address: str) -> int:
        r = t.to_record()
        cur = self.db.execute(
            """INSERT OR IGNORE INTO readings
               (received_at, device_id, seq, uptime_min, battery_soc, battery_mv, temp_c, humidity,
                steps, usb, charging, charge_full, charger_fault, rtc_valid, rssi_best, packets, address, session)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?)""",
            (datetime.now(timezone.utc).isoformat(timespec="seconds"), r["device_id"], r["seq"],
             r["uptime_min"], r["battery_soc"], r["battery_mv"], r["temp_c"], r["humidity"], r["steps"],
             r["usb"], r["charging"], r["charge_full"], r["charger_fault"], r["rtc_valid"], rssi, address,
             self.session),
        )
        self.db.commit()
        return cur.lastrowid

    @staticmethod
    def print_reading(t: Telemetry, rssi: int) -> None:
        def f(v, fmt, unit=""):
            return "n/a" if v is None else f"{v:{fmt}}{unit}"

        charge = "fault" if t.charger_fault else "charging" if t.charging else "full" if t.charge_full else (
            "usb" if t.usb else "battery")
        now = datetime.now().strftime("%H:%M:%S")
        print(f"{now}  {t.device_id}  seq {t.seq:3d}  batt {f(t.battery_soc, 'd', '%')} {f(t.battery_mv, 'd', 'mV')}"
              f"  {f(t.temp_c, '.2f', 'C')} {f(t.humidity, '.1f', '%RH')}  steps {f(t.steps, 'd')}"
              f"  {charge:8s}  up {t.uptime_min}m  rssi {rssi} dBm")

    def print_summary(self) -> None:
        for dev, st in self.stats.items():
            if not st.rssi:
                continue
            avg = sum(st.rssi) / len(st.rssi)
            per_burst = st.packets / st.bursts if st.bursts else 0
            print(f"--- {dev}: {st.bursts} readings, {st.missed} missed, "
                  f"{per_burst:.1f} adverts/burst, RSSI avg {avg:.0f} / best {max(st.rssi)} / worst {min(st.rssi)} dBm")
            st.rssi.clear()


async def main() -> None:
    stop = asyncio.Event()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", type=Path, default=Path(__file__).parent / "data" / "telemetry.db")
    ap.add_argument("--verbose", action="store_true", help="print every advert, not just one per burst")
    ap.add_argument("--summary", type=int, default=300, help="seconds between reception summaries")
    ap.add_argument("--duration", type=float, default=0, help="stop after N seconds (0 = run until Ctrl+C)")
    ap.add_argument("--serial", metavar="COMx", help="read from the ESP32 receiver on this port instead of PC Bluetooth")
    ap.add_argument("--web", metavar="PORT", type=int, nargs="?", const=8080,
                    help="also serve the dashboard (default port 8080)")
    args = ap.parse_args()
    if args.duration:
        asyncio.get_running_loop().call_later(args.duration, lambda: stop.set())

    gw = Gateway(args.db, args.verbose)
    if args.web:
        from webapp import serve
        serve(args.db, args.web, background=True)
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, stop.set)
        except NotImplementedError:   # Windows: Ctrl+C raises KeyboardInterrupt instead
            pass

    source = f"ESP32 receiver on {args.serial}" if args.serial else "PC Bluetooth"
    print(f"Listening for Equimetrics adverts via {source}, logging to {args.db}")
    print("Ctrl+C to stop.\n")

    async def summaries() -> None:
        while not stop.is_set():
            try:
                await asyncio.wait_for(stop.wait(), timeout=args.summary)
            except asyncio.TimeoutError:
                gw.print_summary()

    try:
        if args.serial:
            await run_serial(gw, args.serial, stop, summaries())
        else:
            async with BleakScanner(detection_callback=gw.on_advert, scanning_mode="active"):
                await summaries()
    finally:
        gw.print_summary()


async def run_serial(gw: Gateway, port: str, stop: asyncio.Event, summaries) -> None:
    import serial   # pyserial; only needed in this mode

    ser = serial.Serial(port, 115200, timeout=0.5)
    loop = asyncio.get_running_loop()
    summary_task = asyncio.create_task(summaries)
    try:
        while not stop.is_set():
            raw = await loop.run_in_executor(None, ser.readline)
            if raw:
                gw.on_serial_line(raw.decode("ascii", "replace"))
    finally:
        summary_task.cancel()
        ser.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
