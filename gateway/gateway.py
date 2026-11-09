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
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

from ecg_manager import EcgManager
import equi_protocol
from equi_protocol import COMPANY_ID, DEFAULT_FLEET_KEY, Identity, Telemetry, decode, session_start

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
    device_session INTEGER,             -- device session ID (Unix time it started; bit 31 = random)
    horse_id      TEXT,                 -- 15-digit RFID of the horse for that session, if scanned
    heart_rate    INTEGER,              -- bpm (v4 firmware), NULL = no beats detected
    session_tag   INTEGER               -- v5: low byte of the session ID (device_session filled in
                                        -- once the encrypted identity has been decrypted)
    -- No UNIQUE key: seq and uptime restart at every device boot, so they are not unique over
    -- time. Duplicate adverts within a burst are merged in memory before insert.
);
CREATE INDEX IF NOT EXISTS idx_readings_dev_time ON readings (device_id, received_at);
CREATE TABLE IF NOT EXISTS device_sessions (
    device_id     TEXT    NOT NULL,
    session_id    INTEGER NOT NULL,     -- as sent by the device
    horse_id      TEXT,
    started_at    TEXT,                 -- ISO-8601 UTC from session_id (NULL if the ID is random)
    first_seen    TEXT    NOT NULL,
    last_seen     TEXT    NOT NULL,
    readings      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, session_id)
);
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
        for col in ("session INTEGER", "device_session INTEGER", "horse_id TEXT", "heart_rate INTEGER",
                    "session_tag INTEGER"):
            try:   # databases created before these columns existed
                self.db.execute(f"ALTER TABLE readings ADD COLUMN {col}")
            except sqlite3.OperationalError:
                pass
        self._drop_old_unique_key()
        self.session = int(time.time())
        self.verbose = verbose
        self.lock = threading.Lock()   # the DB connection is shared with web-request threads
        self.ecg = EcgManager(self.db, self.lock)
        self.current: dict[str, tuple[tuple, int]] = {}   # device -> (burst key, row id)
        self.stats: dict[str, DeviceStats] = {}
        self.identity: dict[str, tuple[int, str | None]] = {}   # device -> (session, horse_id)
        self.ecg.identity = self.identity
        self.last_time_sync: dict[str, float] = {}
        self.publisher = None   # MqttPublisher with --mqtt / --mqtt-dry-run

    def _drop_old_unique_key(self) -> None:
        """Early databases had UNIQUE(device_id, seq, uptime_min), which silently dropped readings
        after a device reboot. Rebuild the table without it, keeping all rows."""
        sql = self.db.execute("SELECT sql FROM sqlite_master WHERE name = 'readings'").fetchone()[0]
        if "UNIQUE (device_id, seq, uptime_min)" not in sql:
            return
        cols = [r[1] for r in self.db.execute("PRAGMA table_info(readings)")]
        names = ", ".join(cols)
        self.db.executescript(f"""
            BEGIN;
            ALTER TABLE readings RENAME TO readings_old;
            {SCHEMA}
            INSERT INTO readings ({names}) SELECT {names} FROM readings_old;
            DROP TABLE readings_old;
            COMMIT;""")
        self.db.executescript(SCHEMA)   # the index moved with the renamed table; recreate it
        print("database: removed the old (device, seq, uptime) unique key from 'readings'")

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
        elif parts[0] == "EQECGD" and len(parts) == 3:
            self.ecg.on_data(parts[1], parts[2])
        elif parts[0] == "EQECG" and len(parts) >= 3:
            self.ecg.on_event(parts[1], parts[2], parts[3] if len(parts) > 3 else "")
        elif parts[0] == "EQRX":
            print("receiver ready")
        elif parts[0] == "EQHB" and self.verbose:
            print(f"  receiver heartbeat: up {parts[1]} s, {parts[2]} adverts forwarded")

    def handle(self, address: str, rssi: int, payload: bytes) -> None:
        t = decode(payload)
        if t is None:
            return
        if self.publisher:
            self.publisher.add(address, rssi, payload, t)
        self.ecg.addresses[t.device_id] = address
        if isinstance(t, Identity):
            self.on_identity(t)
            return
        if self.verbose:
            print(f"  adv {t.device_id} seq={t.seq} rssi={rssi}")

        if t.session_tag is not None:
            # v5 only carries the low byte of the session ID; the full ID comes from the encrypted
            # identity (scan response). Until that has been heard the reading has no session yet and
            # is attached later (on_identity backfills by tag).
            ident = self.identity.get(t.device_id)
            if ident and ident[0] & 0xFF == t.session_tag:
                t.session = ident[0]
        key = (t.session if t.session_tag is None else ("tag", t.session_tag), t.seq, t.uptime_min)
        cur = self.current.get(t.device_id)
        st = self.stats.setdefault(t.device_id, DeviceStats())
        if cur and cur[0] == key:
            # Another advert from the same burst: update reception stats only.
            st.packets += 1
            st.rssi.append(rssi)
            with self.lock:
                self.db.execute(
                    "UPDATE readings SET packets = packets + 1, rssi_best = MAX(rssi_best, ?) WHERE id = ?",
                    (rssi, cur[1]),
                )
                self.db.commit()
            return

        # New burst = new reading. Seq gaps only count as losses within one device session.
        if st.last_seq is not None and cur and cur[0][0] == key[0]:
            gap = (t.seq - st.last_seq) % 256
            if gap > 1:
                st.missed += gap - 1
        st.last_seq = t.seq
        st.bursts += 1
        st.packets += 1
        st.rssi.append(rssi)

        row_id = self.store(t, rssi, address)
        self.current[t.device_id] = (key, row_id)
        self.print_reading(t, rssi, self.horse_for(t))
        if not t.rtc_valid or t.no_gateway:
            self.maybe_sync_time(t.device_id, pair=t.rtc_valid)

    def horse_for(self, t: Telemetry) -> str | None:
        ident = self.identity.get(t.device_id)
        return ident[1] if ident and ident[0] == t.session else None

    def on_identity(self, i: Identity) -> None:
        prev = self.identity.get(i.device_id)
        self.identity[i.device_id] = (i.session, i.horse_id)
        if prev == (i.session, i.horse_id):
            return
        print(f"{datetime.now():%H:%M:%S}  {i.device_id}  session {i.session}  HorseID {i.horse_id or '(not scanned)'}")
        with self.lock:
            self._touch_session(i.device_id, i.session, count=False)
            # v5 readings that arrived before the identity: attach them to this session.
            cur = self.db.execute(
                "UPDATE readings SET device_session = ? WHERE device_id = ? AND device_session IS NULL "
                "AND session_tag = ? AND received_at >= ?",
                (i.session, i.device_id, i.session & 0xFF,
                 datetime.fromtimestamp(time.time() - 86400, timezone.utc).isoformat(timespec="seconds")))
            if cur.rowcount:
                self.db.execute("UPDATE device_sessions SET readings = readings + ? WHERE device_id = ? AND "
                                "session_id = ?", (cur.rowcount, i.device_id, i.session))
                print(f"  attached {cur.rowcount} earlier reading(s) to session {i.session}")
            if i.horse_id:
                self.db.execute("UPDATE device_sessions SET horse_id = ? WHERE device_id = ? AND session_id = ?",
                                (i.horse_id, i.device_id, i.session))
                # Readings that arrived before the scan response get the HorseID too.
                self.db.execute("UPDATE readings SET horse_id = ? WHERE device_id = ? AND device_session = ? "
                                "AND horse_id IS NULL", (i.horse_id, i.device_id, i.session))
            self.db.commit()

    def _touch_session(self, device_id: str, session: int, count: bool) -> None:
        now = datetime.now(timezone.utc).isoformat(timespec="seconds")
        start = session_start(session)
        started = None if start is None else datetime.fromtimestamp(start, timezone.utc).isoformat(timespec="seconds")
        self.db.execute(
            "INSERT INTO device_sessions (device_id, session_id, started_at, first_seen, last_seen, readings) "
            "VALUES (?,?,?,?,?,?) ON CONFLICT (device_id, session_id) DO UPDATE SET last_seen = excluded.last_seen, "
            "readings = readings + excluded.readings", (device_id, session, started, now, now, 1 if count else 0))

    def maybe_sync_time(self, device_id: str, pair: bool = False) -> None:
        """Connect and set the device clock from the PC. Done when the RTC has no time, and when the
        device asks for a gateway (advert flag): writing a command makes this gateway trusted, so its
        scan requests then count as receipts (green LED). At most every 2 min per device."""
        if self.ecg.serial is None or time.time() - self.last_time_sync.get(device_id, 0) < 120:
            return
        self.last_time_sync[device_id] = time.time()
        self.ecg.sync_time(device_id, reason="pairing (device asked for a gateway)" if pair else None)

    def store(self, t: Telemetry, rssi: int, address: str) -> int:
        r = t.to_record()
        with self.lock:
            return self._insert(t, r, rssi, address)

    def _insert(self, t: Telemetry, r: dict, rssi: int, address: str) -> int:
        uptime = r["uptime_min"]
        if uptime is None:   # v2: derive from the session start when it is a real timestamp
            sess = r["session"]
            start = None if sess is None else session_start(sess)
            uptime = 0 if start is None else max(0, int(time.time() - start) // 60)
        cur = self.db.execute(
            """INSERT INTO readings
               (received_at, device_id, seq, uptime_min, battery_soc, battery_mv, temp_c, humidity,
                steps, usb, charging, charge_full, charger_fault, rtc_valid, rssi_best, packets, address, session,
                device_session, horse_id, heart_rate, session_tag)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?)""",
            (datetime.now(timezone.utc).isoformat(timespec="seconds"), r["device_id"], r["seq"],
             uptime, r["battery_soc"], r["battery_mv"], r["temp_c"], r["humidity"], r["steps"],
             r["usb"], r["charging"], r["charge_full"], r["charger_fault"], r["rtc_valid"], rssi, address,
             self.session, r["session"], self.horse_for(t), r["heart_rate"], r["session_tag"]),
        )
        if r["session"] is not None:
            self._touch_session(r["device_id"], r["session"], count=True)
        self.db.commit()
        return cur.lastrowid

    @staticmethod
    def print_reading(t: Telemetry, rssi: int, horse: str | None = None) -> None:
        def f(v, fmt, unit=""):
            return "n/a" if v is None else f"{v:{fmt}}{unit}"

        charge = "fault" if t.charger_fault else "charging" if t.charging else "full" if t.charge_full else (
            "usb" if t.usb else "battery")
        now = datetime.now().strftime("%H:%M:%S")
        print(f"{now}  {t.device_id}  seq {t.seq:3d}  batt {f(t.battery_soc, '.1f', '%')} {f(t.battery_mv, 'd', 'mV')}"
              f"  {f(t.temp_c, '.2f', 'C')} {f(t.humidity, '.1f', '%RH')}  steps {f(t.steps, 'd')}"
              f"  HR {f(t.heart_rate, 'd', ' bpm')}"
              f"  {charge:8s}  {'RFID scan  ' if t.rfid_scanning else ''}horse {horse or '-'}  rssi {rssi} dBm")

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
    ap.add_argument("--battery-mah", type=float, default=1500, help="battery capacity for drain/runtime estimates")
    ap.add_argument("--mqtt", metavar="ENDPOINT",
                    help="also publish each burst to AWS IoT Core at this endpoint (IoT Core -> Settings)")
    ap.add_argument("--mqtt-dry-run", action="store_true",
                    help="build the MQTT messages but only write them to --mqtt-dump (no AWS)")
    ap.add_argument("--mqtt-thing", default="equi-pc-gateway-dev",
                    help="IoT thing name: MQTT client ID, gatewayId and topic (default equi-pc-gateway-dev)")
    ap.add_argument("--mqtt-certs", type=Path, default=None,
                    help="folder with certificate.pem.crt, private.pem.key, AmazonRootCA1.pem "
                         "(default <repo>/private/iot)")
    ap.add_argument("--mqtt-dump", type=Path, default=None, metavar="DIR",
                    help="also save every MQTT message as JSON in this folder")
    ap.add_argument("--fleet-key", type=Path, default=DEFAULT_FLEET_KEY,
                    help="16-byte fleet key file for the encrypted identity (default: <repo>/private/fleet_key.bin)")
    args = ap.parse_args()
    if not equi_protocol.load_fleet_key(args.fleet_key):
        print(f"WARNING: fleet key {args.fleet_key} not found - sessions and HorseIDs from v5 firmware "
              "cannot be read\n")
    if args.duration:
        asyncio.get_running_loop().call_later(args.duration, lambda: stop.set())

    gw = Gateway(args.db, args.verbose)
    if args.mqtt or args.mqtt_dry_run:
        from mqtt_publisher import DEFAULT_CERT_DIR, MqttPublisher
        dump = args.mqtt_dump or (Path(__file__).parent / "data" / "mqtt_out" if args.mqtt_dry_run else None)
        gw.publisher = MqttPublisher(args.mqtt_thing, args.mqtt, args.mqtt_certs or DEFAULT_CERT_DIR,
                                     dump_dir=dump, dry_run=args.mqtt_dry_run)
        print(f"MQTT: {'dry run, writing to ' + str(dump) if args.mqtt_dry_run else args.mqtt} "
              f"(topic {gw.publisher.topic})")
    if args.web:
        from webapp import serve
        serve(args.db, args.web, background=True, ecg=gw.ecg, battery_mah=args.battery_mah)
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
        if gw.publisher:
            gw.publisher.close()
            print(f"MQTT: {gw.publisher.sent} messages sent, {gw.publisher.failed} failed")


async def run_serial(gw: Gateway, port: str, stop: asyncio.Event, summaries) -> None:
    import serial   # pyserial; only needed in this mode

    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException as e:
        summaries.close()
        raise SystemExit(f"\nCannot open {port}: {e}\n"
                         f"Is another program using it (another gateway, a serial monitor, idf.py monitor)? "
                         f"Close it and try again.")
    gw.ecg.serial = ser   # lets the web app request ECG captures through the receiver
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
