"""Decoder for the Equimetrics VPro BLE telemetry advert.

Mirrors equi-sensordevice-vpro/components/telemetry/include/telemetry.h.
Manufacturer Specific Data, company ID 0xFFFF (test ID - replace for production).
"""
from __future__ import annotations

import struct
from dataclasses import asdict, dataclass
from typing import Optional

COMPANY_ID = 0xFFFF
MAGIC = 0xE9

FLAG_USB = 1 << 0
FLAG_CHARGING = 1 << 1
FLAG_FULL = 1 << 2
FLAG_FAULT = 1 << 3
FLAG_RTC_VALID = 1 << 4


@dataclass
class Telemetry:
    device_id: str
    seq: int
    usb: bool
    charging: bool
    charge_full: bool
    charger_fault: bool
    rtc_valid: bool
    battery_soc: Optional[int]      # %
    battery_mv: Optional[int]
    temp_c: Optional[float]
    humidity: Optional[float]       # %RH
    steps: Optional[int]
    uptime_min: int
    version: int

    def to_record(self) -> dict:
        """Plain dict, the same shape we would later publish to MQTT / AWS."""
        return asdict(self)


def decode(payload: bytes) -> Optional[Telemetry]:
    """Decode the bytes after the company ID. Returns None if it is not ours."""
    if len(payload) < 2 or payload[0] != MAGIC:
        return None
    version = payload[1]
    if version != 1 or len(payload) < 19:
        return None   # unknown/newer format: ignore rather than mis-decode

    dev = payload[2:5].hex().upper()
    seq, flags, soc = payload[5], payload[6], payload[7]
    mv, temp, = struct.unpack_from("<Hh", payload, 8)
    rh_half = payload[12]
    steps, uptime = struct.unpack_from("<IH", payload, 13)

    return Telemetry(
        device_id=dev,
        seq=seq,
        usb=bool(flags & FLAG_USB),
        charging=bool(flags & FLAG_CHARGING),
        charge_full=bool(flags & FLAG_FULL),
        charger_fault=bool(flags & FLAG_FAULT),
        rtc_valid=bool(flags & FLAG_RTC_VALID),
        battery_soc=None if soc == 0xFF else soc,
        battery_mv=None if mv == 0 else mv,
        temp_c=None if temp == 0x7FFF else temp / 100.0,
        humidity=None if rh_half == 0xFF else rh_half / 2.0,
        steps=None if steps == 0xFFFFFFFF else steps,
        uptime_min=uptime,
        version=version,
    )


if __name__ == "__main__":
    # Self-test with a payload built the same way as the firmware's telemetry_encode().
    p = bytes([MAGIC, 1, 0x15, 0xB0, 0xB4, 7, FLAG_USB | FLAG_FULL | FLAG_RTC_VALID, 99])
    p += struct.pack("<HhB", 4181, 2416, 102) + struct.pack("<IH", 150, 3)
    t = decode(p)
    assert t and t.device_id == "15B0B4" and t.temp_c == 24.16 and t.humidity == 51.0 and t.steps == 150
    print("decode self-test OK:", t.to_record())
