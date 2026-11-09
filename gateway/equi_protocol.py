"""Decoder for the Equimetrics VPro BLE adverts.

Mirrors equi-sensordevice-vpro/components/telemetry/include/telemetry.h.
Manufacturer Specific Data, company ID 0xFFFF (test ID - replace for production).

  E9 01 ...  telemetry v1 (uptime, no session)       - older firmware
  E9 02 ...  telemetry v2 (session ID, 30 s data)    - advert (whole-% battery)
  E9 03 ...  telemetry v3 (as v2, battery in 1/256 %) - advert
  E9 04 ...  telemetry v4 (v3 + heart rate)            - advert
  E9 05 ...  telemetry v5 (1-byte session tag)         - advert
  E9 81 ...  identity v1 (session ID, HorseID, clear) - scan response (older firmware)
  E9 82 ...  identity v2 (AES-CCM, 4-byte session s)   - scan response (older firmware)
  E9 83 ...  identity v3 (AES-CCM, 8-byte session ms)  - scan response, needs the fleet key

Session IDs: older firmware used Unix seconds (bit 31 = random); current firmware uses Unix
milliseconds, as the VPro platform does (bit 52 = random). Use session_is_random() and
session_start() rather than testing bits directly.
"""
from __future__ import annotations

import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional, Union

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

COMPANY_ID = 0xFFFF
MAGIC = 0xE9

FLAG_USB = 1 << 0
FLAG_CHARGING = 1 << 1
FLAG_FULL = 1 << 2
FLAG_FAULT = 1 << 3
FLAG_RTC_VALID = 1 << 4
FLAG_RFID_SCAN = 1 << 5
FLAG_NO_GATEWAY = 1 << 6   # no trusted gateway heard it lately: connect + write to pair

SESSION_RANDOM_BIT = 1 << 52      # ms session is random (device RTC had no time), not a timestamp
SESSION_RANDOM_BIT_S = 1 << 31    # same, for older firmware's seconds session IDs
_MS_THRESHOLD = 10 ** 11          # IDs above this are milliseconds (10^11 s is the year 5138)


def session_is_random(session: int) -> bool:
    if session < 1 << 32:
        return bool(session & SESSION_RANDOM_BIT_S)
    return bool(session & SESSION_RANDOM_BIT)


def session_start(session: int) -> Optional[float]:
    """Unix time (s) the session started, or None if the ID is random."""
    if session_is_random(session):
        return None
    return session / 1000 if session > _MS_THRESHOLD else float(session)

# Fleet key (16 bytes) for the encrypted identity. Kept in <repo>/private/ (git-ignored), the
# same file the firmware embeds. Never commit it or send it anywhere.
DEFAULT_FLEET_KEY = Path(__file__).resolve().parent.parent / "private" / "fleet_key.bin"
_ccm: Optional[AESCCM] = None
bad_identity = 0   # identities that failed to decrypt (wrong fleet key / corrupted)


def set_fleet_key(key: bytes) -> None:
    global _ccm
    if len(key) != 16:
        raise ValueError("fleet key must be 16 bytes")
    _ccm = AESCCM(key, tag_length=4)


def load_fleet_key(path: Path = DEFAULT_FLEET_KEY) -> bool:
    """Load the fleet key; returns False (identities stay unreadable) if the file is missing."""
    if not path.exists():
        return False
    set_fleet_key(path.read_bytes())
    return True


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
    session: Optional[int]          # v2-v4: Unix time the session started (bit 31 = random);
                                    # v5: filled in by the gateway from the identity (else None)
    session_tag: Optional[int]      # v5: low byte of the session ID
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
    global bad_identity
    if len(payload) < 2 or payload[0] != MAGIC:
        return None
    kind = payload[1]
    dev = payload[2:5].hex().upper() if len(payload) >= 5 else ""
    hr = None

    if kind == 0x83 and len(payload) >= 27:
        if _ccm is None:
            return None
        try:
            pt = _ccm.decrypt(payload[2:9], payload[9:27], payload[0:9])
        except InvalidTag:
            bad_identity += 1
            return None
        session, = struct.unpack_from("<Q", pt, 0)
        return Identity(device_id=dev, session=session, horse_id=horse_id_str(int.from_bytes(pt[8:14], "little")))

    if kind == 0x82 and len(payload) >= 23:
        if _ccm is None:
            return None   # no fleet key loaded: cannot read it
        nonce = payload[2:9]   # device_id + random
        try:
            pt = _ccm.decrypt(nonce, payload[9:23], payload[0:9])
        except InvalidTag:
            bad_identity += 1
            return None
        session, = struct.unpack_from("<I", pt, 0)
        return Identity(device_id=dev, session=session, horse_id=horse_id_str(int.from_bytes(pt[4:10], "little")))

    tag = None
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
    elif kind == 5 and len(payload) >= 20:
        tag, seq, flags = payload[5], payload[6], payload[7]
        soc256, mv, temp = struct.unpack_from("<HHh", payload, 8)
        soc = 0xFF if soc256 == 0xFFFF else round(soc256 / 256.0, 3)
        rh_half = payload[14]
        steps, = struct.unpack_from("<I", payload, 15)
        hr = payload[19] or None
        session = uptime = None
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
             steps=None if steps == 0xFFFFFFFF else steps, heart_rate=hr, session=session, session_tag=tag, uptime_min=uptime,
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
    # v5 telemetry + encrypted identity, built like the firmware does
    p = bytes([MAGIC, 5, 0x15, 0xB0, 0xB4, 1794100000 & 0xFF, 9, FLAG_RTC_VALID])
    p += struct.pack("<HHhB", round(50 * 256), 3900, 2100, 90) + struct.pack("<I", 7) + bytes([41])
    t5 = decode(p)
    assert isinstance(t5, Telemetry) and t5.version == 5 and t5.session is None and t5.heart_rate == 41
    assert t5.session_tag == 1794100000 & 0xFF and t5.steps == 7 and t5.battery_mv == 3900, t5
    import os
    set_fleet_key(os.urandom(16))
    hdr = bytes([MAGIC, 0x82, 0x15, 0xB0, 0xB4]) + os.urandom(4)
    ct = _ccm.encrypt(hdr[2:9], struct.pack("<I", 1794100000) + packed.to_bytes(6, "little"), hdr)
    i2 = decode(hdr + ct)
    assert isinstance(i2, Identity) and i2.session == 1794100000 and i2.horse_id == "900002600096932", i2
    assert decode(hdr + bytes([ct[0] ^ 1]) + ct[1:]) is None and bad_identity == 1   # tampered
    hdr3 = bytes([MAGIC, 0x83, 0x15, 0xB0, 0xB4]) + os.urandom(4)
    ct3 = _ccm.encrypt(hdr3[2:9], struct.pack("<Q", 1794100000123) + packed.to_bytes(6, "little"), hdr3)
    i3 = decode(hdr3 + ct3)
    assert isinstance(i3, Identity) and i3.session == 1794100000123 and i3.horse_id == "900002600096932", i3
    assert session_start(1794100000123) == 1794100000.123 and session_start(1794100000) == 1794100000
    assert session_is_random((1 << 52) | 5) and session_is_random((1 << 31) | 5) and not session_is_random(1794100000123)
    print("decode self-test OK:", t.to_record(), i, t5, i2)
