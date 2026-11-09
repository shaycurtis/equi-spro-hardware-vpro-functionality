"""
End-to-end offline test: receiver serial lines -> PC gateway (real Gateway class, temporary DB)
-> MQTT publisher (dry run, writes the messages) -> postBleData (fake API). Nothing leaves the PC.

    python -m unittest cloud/post_ble_data/test_gateway_to_lambda.py -v
"""
import json
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
GW = HERE.parent.parent / "gateway"
sys.path[:0] = [str(HERE), str(GW)]

import equi_protocol as proto     # noqa: E402
import gateway as gwmod           # noqa: E402
import mqtt_publisher             # noqa: E402
import post_ble_data as lam       # noqa: E402

KEY = bytes(range(16))            # test key, not the fleet key
DEV = bytes([0x15, 0xB0, 0xB4])
SESSION = 1794220451000
HORSE = "999012345678436"


def telemetry(seq, tag=SESSION & 0xFF):
    p = bytes([proto.MAGIC, 5]) + DEV + bytes([tag, seq, proto.FLAG_RTC_VALID])
    return p + struct.pack("<HHhB", round(80 * 256), 3900, 2250, 48) + struct.pack("<I", 77) + bytes([44])


def identity():
    hdr = bytes([proto.MAGIC, 0x83]) + DEV + b"\x09\x08\x07\x06"
    pt = struct.pack("<Q", SESSION) + ((int(HORSE[:3]) << 38) | int(HORSE[3:])).to_bytes(6, "little")
    return hdr + proto._ccm.encrypt(hdr[2:9], pt, hdr)


class Api:
    def __init__(self):
        self.posted = []

    def get_org_id(self, d):
        return "org-1" if d == "15B0B4" else None

    def post_measurements(self, d):
        self.posted.append(d)
        return 1


class GatewayToLambda(unittest.TestCase):
    def test_bursts_become_one_measurement_each(self):
        proto.set_fleet_key(KEY)
        mqtt_publisher.BURST_SETTLE_S = 0.2
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            g = gwmod.Gateway(tmp / "t.db", verbose=False)
            g.publisher = mqtt_publisher.MqttPublisher("equi-pc-gateway-dev", dump_dir=tmp / "out", dry_run=True)
            addr = "50:78:7D:15:B0:B8"
            # burst 1: identity + 3 copies; burst 2 (30 s later): 2 copies, no new identity heard
            g.on_serial_line(f"EQADV,{addr},-70,{telemetry(10).hex()}")
            g.on_serial_line(f"EQADV,{addr},-62,{identity().hex()}")
            g.on_serial_line(f"EQADV,{addr},-60,{telemetry(10).hex()}")
            g.on_serial_line(f"EQADV,{addr},-66,{telemetry(10).hex()}")
            time.sleep(0.8)
            g.on_serial_line(f"EQADV,{addr},-75,{telemetry(11).hex()}")
            g.on_serial_line(f"EQADV,{addr},-73,{telemetry(11).hex()}")
            time.sleep(0.8)
            g.publisher.close()
            msgs = sorted((tmp / "out").glob("*.json"))
            self.assertEqual(len(msgs), 2)
            api = Api()
            for m in msgs:
                ev = json.loads(m.read_text())
                self.assertEqual(ev["gatewayId"], "equi-pc-gateway-dev")
                self.assertEqual(lam.process(ev, api, {})["posted"], 1)
            g.db.close()
        a, b = api.posted
        self.assertEqual((a["sessionId"], a["horseId"], a["signalStrength"]), (SESSION, HORSE, "high"))
        self.assertEqual(b["sessionId"], SESSION)            # identity carried over to burst 2
        self.assertEqual(b["signalStrength"], "medium")      # best of -75 / -73
        self.assertEqual((a["deviceId"], a["heartRate"], a["steps"], a["batteryLevel"]), ("15B0B4", 44, 77, 80))


if __name__ == "__main__":
    unittest.main()
