"""
Write sample_event.json: one 60 s telemetry burst from a device, as a gateway would forward it
(three copies of the advert + the encrypted identity scan response), for the Lambda console's
Test tab. The identity is encrypted with the real fleet key from <repo>/private/fleet_key.bin;
the output only holds the over-the-air bytes, never the key.

    python cloud/post_ble_data/make_sample_event.py [--device 15B0B4] [--horse 999012345678436]
"""
import argparse
import json
import os
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "gateway"))
import equi_protocol as proto   # noqa: E402


def ad(ad_type: int, data: bytes) -> bytes:
    return bytes([1 + len(data), ad_type]) + data


def mfg(payload: bytes) -> bytes:
    return ad(0xFF, struct.pack("<H", proto.COMPANY_ID) + payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="15B0B4")
    ap.add_argument("--horse", default="999012345678436")
    ap.add_argument("--out", default=str(HERE / "sample_event.json"))
    a = ap.parse_args()
    if not proto.load_fleet_key():
        sys.exit("private/fleet_key.bin not found")

    dev = bytes.fromhex(a.device)
    now = int(time.time() * 1000)
    session = now - 20 * 60 * 1000                       # a session that started 20 min ago
    tag = session & 0xFF

    # telemetry v6, 23 bytes: battery 86.5 %, 3.95 V, 23.10 C, 50 %RH, 850 steps, 38 bpm,
    # on battery, standing now (4), moving + standing during the minute, probe 37.50 C.
    # The probe is the last int16 (0x7FFF would mean no probe). A console test of the zip
    # from build.py should show horseTemperature 37.50, not the board's 23.10.
    p = bytes([proto.MAGIC, 6]) + dev + bytes([tag, 42, proto.FLAG_RTC_VALID])
    p += struct.pack("<HHhB", round(86.5 * 256), 3950, 2310, 50) + struct.pack("<I", 850) + bytes([38])
    p += bytes([4 | (1 << 6) | (1 << 7)]) + struct.pack("<h", 3750)
    adv = (ad(0x01, b"\x06") + mfg(p)).hex()

    # identity v3, encrypted as the firmware does (nonce = device id + 4 random bytes)
    hdr = bytes([proto.MAGIC, 0x83]) + dev + os.urandom(4)
    packed = (int(a.horse[:3]) << 38) | int(a.horse[3:])
    pt = struct.pack("<Q", session) + packed.to_bytes(6, "little")
    rsp = mfg(hdr + proto._ccm.encrypt(hdr[2:9], pt, hdr)).hex()

    mac = "50:78:7D:15:B0:B9"
    event = {"gatewayId": "PC-TEST", "items": [
        {"mac": mac, "rssi": -72, "ts": now, "adv": adv},
        {"mac": mac, "rssi": -64, "ts": now + 100, "adv": adv, "rsp": rsp},
        {"mac": mac, "rssi": -69, "ts": now + 200, "adv": adv},
    ]}
    Path(a.out).write_text(json.dumps(event, indent=2))
    print(f"wrote {a.out}: device {a.device}, session {session}, horse {a.horse}")


if __name__ == "__main__":
    main()
