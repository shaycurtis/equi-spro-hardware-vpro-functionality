"""Print a VPro device's Wi-Fi setup code (provisioning proof of possession + setup network password).

    python tools/setup_code.py 15B0B4

Same derivation as the firmware (telemetry_device_setup_code):
first 8 hex digits of SHA-256(fleet key | "EQ-POP" | device id as 3 big-endian bytes).
Reads the fleet key from <repo>/private/fleet_key.bin (never commit or share it).
"""
import hashlib
import json
import sys
from pathlib import Path

KEY = Path(__file__).resolve().parent.parent / "private" / "fleet_key.bin"


def setup_code(device_id: str, key: bytes) -> str:
    dev = int(device_id, 16).to_bytes(3, "big")
    return hashlib.sha256(key + b"EQ-POP" + dev).hexdigest()[:8]


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dev = sys.argv[1].upper()
    code = setup_code(dev, KEY.read_bytes())
    name = f"PROV_{dev}"
    print(f"Setup network: {name}   password: {code}   proof of possession: {code}")
    print("QR payload:", json.dumps({"ver": "v1", "name": name, "pop": code, "transport": "softap",
                                     "password": code}, separators=(",", ":")))
