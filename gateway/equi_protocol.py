"""Decoder for the Equimetrics VPro BLE adverts.

Mirrors equi-sensordevice-vpro/components/telemetry/include/telemetry.h.
Manufacturer Specific Data, company ID 0xFFFF (test ID - replace for production).

  E9 01 ...  telemetry v1 (uptime, no session)       - older firmware
  E9 02 ...  telemetry v2 (session ID, 30 s data)    - advert (whole-% battery)
  E9 03 ...  telemetry v3 (as v2, battery in 1/256 %) - advert
  E9 04 ...  telemetry v4 (v3 + heart rate)            - advert
  E9 81 ...  identity v1 (session ID, HorseID)       - scan response
"""
from __future__ import annotations

import struct
from dataclasses import asdict, dataclass
from typing import Optional, Union

COMPANY_ID = 0xFFFF
MAGIC = 0xE9

FLAG_USB = 1 << 0
FLAG_CHARGING = 1 << 1
FLAG_FULL = 1 << 2
FLAG_FAULT = 1 << 3
FLAG_RTC_VALID = 1 << 4
FLAG_RFID_SCAN = 1 << 5
FLAG_NO_GATEWAY = 1 << 6   # no trusted gateway heard it lately: connect + write to pair

SESSION_RANDOM_BIT = 1 << 31   # session is random (device RTC had no time), not a timestamp


@dataclass
class Telemetry:
    device_id: str
    seq: int
    usb: bool
    charging: bool
    charge_full: bool
    charger_fault: bool
    rtc_valid: bool
    rfid_scanning: bool
    no_gateway: bool
    battery_soc: Optional[float]    # % (v3: 1/256 % resolution)
    battery_mv: Optional[int]
    temp_c: Optional[float]
    humidity: Optional[float]       # %RH
    steps: Optional[int]
    heart_rate: Optional[int]       # bpm (v4+), None = no beats / older firmware
    session: Optional[int]          # v2+: Unix time the session started (bit 31 = random)
    uptime_min: Optional[int]       # v1 only
    version: int

    def to_record(self) -> dict:
        """Plain dict, the same shape we would later publish to MQTT / AWS."""
        return asdict(self)


@dataclass
class Identity:
    device_id: str
    session: int
    horse_id: Optional[str]         # 15 digits, e.g. "985141000123456"; None = no tag scanned


def horse_id_str(packed: int) -> Optional[str]:
    """ISO 11784 packing (country << 38 | national) -> 15-digit string."""
    if not packed:
        return None
    return f"{(packed >> 38) & 0x3FF:03d}{packed & ((1 << 38) - 1):012d}"


def _flags(t: dict, flags: int) -> None:
    t.update(usb=bool(flags & FLAG_USB), charging=bool(flags & FLAG_CHARGING), charge_full=bool(flags & FLAG_FULL),
             charger_fault=bool(flags & FLAG_FAULT), rtc_valid=bool(flags & FLAG_RTC_VALID),
             rfid_scanning=bool(flags & FLAG_RFID_SCAN), no_gateway=bool(flags & FLAG_NO_GATEWAY))


def decode(payload: bytes) -> Union[Telemetry, Identity, None]:
    """Decode the bytes after the company ID. Returns None if it is not ours / unknown."""
    if len(payload) < 2 or payload[0] != MAGIC:
        return None
    kind = payload[1]
    dev = payload[2:5].hex().upper() if len(payload) >= 5 else ""
    hr = None

    if kind == 0x81 and len(payload) >= 15:
        session, = struct.unpack_from("<I", payload, 5)
        packed = int.from_bytes(payload[9:15], "little")
        return Identity(device_id=dev, session=session, horse_id=horse_id_str(packed))

    if kind == 1 and len(payload) >= 19:
        seq, flags, soc = payload[5], payload[6], payload[7]
        mv, temp = struct.unpack_from("<Hh", payload, 8)
        rh_half = payload[12]
        steps, uptime = struct.unpack_from("<IH", payload, 13)
        session = None
    elif kind == 2 and len(payload) >= 21:
        session, = struct.unpack_from("<I", payload, 5)
        seq, flags, soc = payload[9], payload[10], payload[11]
        mv, temp = struct.unpack_from("<Hh", payload, 12)
        rh_half = payload[16]
        steps, = struct.unpack_from("<I", payload, 17)
        uptime = None
    elif kind in (3, 4) and len(payload) >= 22:
        session, = struct.unpack_from("<I", payload, 5)
        seq, flags = payload[9], payload[10]
        soc256, mv, temp = struct.unpack_from("<HHh", payload, 11)
        soc = 0xFF if soc256 == 0xFFFF else round(soc256 / 256.0, 3)
        rh_half = payload[17]
        steps, = struct.unpack_from("<I", payload, 18)
        uptime = None
        if kind == 4 and len(payload) >= 23 and payload[22]:
            hr = payload[22]
    else:
        return None   # unknown/newer format: ignore rather than mis-decode

    t = dict(device_id=dev, seq=seq, battery_soc=None if soc == 0xFF else soc,
             battery_mv=None if mv == 0 else mv, temp_c=None if temp == 0x7FFF else temp / 100.0,
             humidity=None if rh_half == 0xFF else rh_half / 2.0,
             steps=None if steps == 0xFFFFFFFF else steps, heart_rate=hr, session=session, uptime_min=uptime,
             version=kind)
    _flags(t, flags)
    return Telemetry(**t)


if __name__ == "__main__":
    # v4 telemetry built the same way as the firmware's telemetry_encode()
    p = bytes([MAGIC, 4, 0x15, 0xB0, 0xB4]) + struct.pack("<I", 1794100000)
    p += bytes([7, FLAG_USB | FLAG_FULL | FLAG_RTC_VALID]) + struct.pack("<HHhB", round(87.25 * 256), 4181, 2416, 102)
    p += struct.pack("<I", 150) + bytes([62])
    t = decode(p)
    assert isinstance(t, Telemetry) and t.session == 1794100000 and t.temp_c == 24.16 and t.steps == 150
    assert t.battery_soc == 87.25 and t.version == 4 and t.heart_rate == 62, t
    # identity with the manual's example FDX-B tag 900002600096932
    packed = (900 << 38) | 2600096932
    i = decode(bytes([MAGIC, 0x81, 0x15, 0xB0, 0xB4]) + struct.pack("<I", 1794100000) + packed.to_bytes(6, "little"))
    assert isinstance(i, Identity) and i.horse_id == "900002600096932", i
    print("decode self-test OK:", t.to_record(), i)
