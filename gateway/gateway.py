"""Equimetrics PC BLE gateway (development).

Scans for VPro telemetry adverts, de-duplicates each 2 s burst (one reading per
device+seq), prints it, and stores it in SQLite. Also tracks reception quality for
range testing: adverts heard per burst, best RSSI, and missed bursts (seq gaps).

    python gateway.py                 # scan and log to data/telemetry.db
    python gateway.py --verbose       # also print every individual advert
"""
from __future__ import annotations

import argparse
import asyncio
import signal
import sqlite3
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
        self.db = sqlite3.connect(db_path)
        self.db.executescript(SCHEMA)
        self.verbose = verbose
        self.current: dict[str, tuple[int, int, int]] = {}   # device -> (seq, uptime, row id)
        self.stats: dict[str, DeviceStats] = {}

    def on_advert(self, device: BLEDevice, adv: AdvertisementData) -> None:
        payload = adv.manufacturer_data.get(COMPANY_ID)
        if not payload:
            return
        t = decode(payload)
        if t is None:
            return
        rssi = adv.rssi
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

        row_id = self.store(t, rssi, device.address)
        self.current[t.device_id] = (t.seq, t.uptime_min, row_id)
        self.print_reading(t, rssi)

    def store(self, t: Telemetry, rssi: int, address: str) -> int:
        r = t.to_record()
        cur = self.db.execute(
            """INSERT OR IGNORE INTO readings
               (received_at, device_id, seq, uptime_min, battery_soc, battery_mv, temp_c, humidity,
                steps, usb, charging, charge_full, charger_fault, rtc_valid, rssi_best, packets, address)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?)""",
            (datetime.now(timezone.utc).isoformat(timespec="seconds"), r["device_id"], r["seq"],
             r["uptime_min"], r["battery_soc"], r["battery_mv"], r["temp_c"], r["humidity"], r["steps"],
             r["usb"], r["charging"], r["charge_full"], r["charger_fault"], r["rtc_valid"], rssi, address),
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
    args = ap.parse_args()
    if args.duration:
        asyncio.get_running_loop().call_later(args.duration, lambda: stop.set())

    gw = Gateway(args.db, args.verbose)
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, stop.set)
        except NotImplementedError:   # Windows: Ctrl+C raises KeyboardInterrupt instead
            pass

    print(f"Scanning for Equimetrics adverts (company 0x{COMPANY_ID:04X}), logging to {args.db}")
    print("Ctrl+C to stop.\n")
    async with BleakScanner(detection_callback=gw.on_advert, scanning_mode="active"):
        try:
            while not stop.is_set():
                try:
                    await asyncio.wait_for(stop.wait(), timeout=args.summary)
                except asyncio.TimeoutError:
                    gw.print_summary()
        finally:
            gw.print_summary()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
