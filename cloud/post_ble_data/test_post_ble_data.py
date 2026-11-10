"""
Offline tests for postBleData: adverts built the way the firmware builds them, a fake Secrets
Manager and a fake EquimetricsApi (nothing is sent anywhere).

    python -m unittest cloud/post_ble_data/test_post_ble_data.py
"""
import json
import os
import struct
import sys
import unittest
from decimal import Decimal
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "gateway"))   # equi_protocol.py (packaged with the Lambda)

import equi_protocol as proto            # noqa: E402
import post_ble_data as lam              # noqa: E402

TEST_KEY = bytes(range(16))              # test-only key, not the fleet key
DEV = bytes([0x15, 0xB0, 0xB4])
SESSION = 1794220451000                  # ms
HORSE = "900002600096932"


def horse_packed(s: str) -> int:
    return (int(s[:3]) << 38) | int(s[3:])


def ad(ad_type: int, data: bytes) -> bytes:
    return bytes([1 + len(data), ad_type]) + data


def mfg(payload: bytes) -> bytes:
    return ad(0xFF, struct.pack("<H", proto.COMPANY_ID) + payload)


def telemetry_adv(seq: int, tag: int = SESSION & 0xFF, flags: int = proto.FLAG_RTC_VALID, soc=87.25,
                  mv=3990, temp=22.91, rh=51, steps=1234, hr=41, pos="standing", seen=("standing",),
                  version=6) -> str:
    """Advert v6 as the firmware's telemetry_encode() builds it (version=5: without position)."""
    p = bytes([proto.MAGIC, version]) + DEV + bytes([tag, seq, flags])
    p += struct.pack("<HHhB", round(soc * 256), mv, round(temp * 100), rh) + struct.pack("<I", steps) + bytes([hr])
    if version >= 6:
        b = proto.POSITIONS.index(pos)
        for s in seen:
            b |= 1 << (proto.POSITIONS.index(s) + 2)
        p += bytes([b])
    return (ad(0x01, bytes([0x06])) + mfg(p)).hex()


def identity_rsp(session: int = SESSION, horse: str = HORSE, nonce: bytes = b"\x01\x02\x03\x04") -> str:
    hdr = bytes([proto.MAGIC, 0x83]) + DEV + nonce
    pt = struct.pack("<Q", session) + horse_packed(horse).to_bytes(6, "little")
    return mfg(hdr + proto._ccm.encrypt(hdr[2:9], pt, hdr)).hex()


class FakeApi:
    def __init__(self, orgs):
        self.orgs, self.posted, self.lookups = orgs, [], []

    def get_org_id(self, device_id):
        self.lookups.append(device_id)
        return self.orgs.get(device_id)

    def post_measurements(self, data):
        self.posted.append(data)
        return object()   # a "response"


class FakeSecrets:
    def get_secret_value(self, SecretId):
        assert SecretId == "vpro/dev/ble-fleet-key"
        return {"SecretString": json.dumps({"fleetKey": TEST_KEY.hex()})}


MAP = {}                                  # default: EUI-64 from the advert MAC + short ID
ORGS = {"50787DFFFE15B0B4": "org-123", "15B0B4": "org-123"}   # short: items without a MAC


class PostBleDataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lam._fleet_key_loaded = False
        lam.load_fleet_key(FakeSecrets())

    def test_one_burst_posts_once_with_identity(self):
        # A 2 s burst: the same advert heard 3 times, plus the scan response.
        ev = {"gatewayId": "PC-TEST", "items": [
            {"mac": "50:78:7D:15:B0:B8", "rssi": -70, "ts": 1794220451200, "adv": telemetry_adv(9)},
            {"mac": "50:78:7D:15:B0:B8", "rssi": -61, "ts": 1794220451300, "adv": telemetry_adv(9),
             "rsp": identity_rsp()},
            {"mac": "50:78:7D:15:B0:B8", "rssi": -66, "ts": 1794220451100, "adv": telemetry_adv(9)},
        ]}
        api = FakeApi(ORGS)
        stats = lam.process(ev, api, MAP)
        self.assertEqual(stats, {"readings": 1, "posted": 1, "skipped": 0})
        d = api.posted[0]
        self.assertEqual(d["deviceId"], "50787DFFFE15B0B4")   # OUI of 50:78:7D:15:B0:B8 + FFFE + 15B0B4
        self.assertEqual(d["organisationId"], "org-123")
        self.assertEqual(d["sessionId"], SESSION)
        self.assertEqual(d["horseId"], HORSE)
        self.assertEqual(d["timestamp"], 1794220451100)        # first copy
        self.assertEqual(d["signalStrength"], "high")          # best RSSI -61
        self.assertEqual(d["batteryLevel"], 87)
        self.assertEqual(d["temperature"], Decimal("22.91"))
        self.assertEqual(d["heartRate"], 41)
        self.assertEqual(d["steps"], 1234)
        self.assertEqual(d["status"], "Disconnected")
        self.assertIs(d["wifiConnected"], False)
        # temporary placeholders for the app's horse charts
        self.assertEqual(d["horseTemperature"], Decimal("22.91"))
        self.assertEqual(d["respiration"], 14)                 # round(41 / 3)
        # position from the advert; the current one is not repeated in positionsSeen
        self.assertEqual((d["position"], d["positionsSeen"]), ("standing", []))

    def post_one(self, **adv):
        ev = {"gatewayId": "G", "items": [{"rssi": -60, "ts": 1, "adv": telemetry_adv(7, **adv),
                                           "rsp": identity_rsp()}]}
        api = FakeApi(ORGS)
        self.assertEqual(lam.process(ev, api, MAP)["posted"], 1)
        return api.posted[0]

    def test_position_and_positions_seen(self):
        # lying on a side now; stood, went down on the chest, then over during the minute
        d = self.post_one(pos="lateral", seen=("standing", "sternal", "lateral"))
        self.assertEqual(d["position"], "lateral")
        self.assertEqual(d["positionsSeen"], ["sternal", "standing"])     # VPro list order
        d = self.post_one(pos="moving", seen=("moving", "standing", "rolling"))
        self.assertEqual((d["position"], d["positionsSeen"]), ("moving", ["rolling", "standing"]))
        # no fix yet (just switched on): unknown, as the LoRa parser's index 0
        d = self.post_one(pos="unknown", seen=())
        self.assertEqual((d["position"], d["positionsSeen"]), ("unknown", []))

    def test_firmware_without_position_keeps_the_placeholder(self):
        d = self.post_one(version=5)
        self.assertEqual((d["position"], d["positionsSeen"]), ("standing", []))
        self.assertEqual((d["heartRate"], d["steps"]), (41, 1234))

    def test_no_heart_rate_is_zero_not_null(self):
        from types import SimpleNamespace
        for hr in (None, 0):
            m = lam.placeholder_horse_metrics(SimpleNamespace(heart_rate=hr, temp_c=21.5))
            self.assertEqual((m["heartRate"], m["respiration"]), (0, 0))

    def test_shape_matches_create_measurement_data(self):
        # Every key we send must be one the common layer forwards (it drops unknown keys).
        known = {"deviceId", "timestamp", "batteryLevel", "fPort", "gatewayId", "heartRate", "horseId",
                 "horseTemperature", "lastKnownLatitude", "lastKnownLongitude", "organisationId",
                 "perspiration", "position", "positionsSeen", "respiration", "sessionId",
                 "signalStrength", "status", "steps", "temperature", "wifiConnected"}
        ev = {"gatewayId": "G", "items": [{"rssi": -80, "ts": 1, "adv": telemetry_adv(1), "rsp": identity_rsp()}]}
        api = FakeApi(ORGS)
        lam.process(ev, api, MAP)
        self.assertLessEqual(set(api.posted[0]), known)

    def test_charging_status(self):
        for flags, want in ((proto.FLAG_USB | proto.FLAG_CHARGING, "Charging"),
                            (proto.FLAG_USB | proto.FLAG_FULL, "Connected"), (0, "Disconnected")):
            ev = {"gatewayId": "G", "items": [{"rssi": -90, "ts": 1, "adv": telemetry_adv(2, flags=flags),
                                               "rsp": identity_rsp()}]}
            api = FakeApi(ORGS)
            lam.process(ev, api, MAP)
            self.assertEqual(api.posted[0]["status"], want)
            self.assertEqual(api.posted[0]["signalStrength"], "low")

    def test_no_session_is_skipped_unless_asked(self):
        ev = {"gatewayId": "G", "items": [{"rssi": -60, "ts": 1, "adv": telemetry_adv(3, tag=0)}]}
        api = FakeApi(ORGS)
        self.assertEqual(lam.process(ev, api, MAP)["posted"], 0)
        api = FakeApi(ORGS)
        self.assertEqual(lam.process(ev, api, MAP, post_without_session=True)["posted"], 1)
        self.assertNotIn("sessionId", api.posted[0])

    def test_identity_for_another_session_is_not_used(self):
        ev = {"gatewayId": "G", "items": [{"rssi": -60, "ts": 1, "adv": telemetry_adv(4),
                                           "rsp": identity_rsp(session=SESSION + 1)}]}
        self.assertEqual(lam.process(ev, FakeApi(ORGS), MAP)["posted"], 0)

    def test_unregistered_devices_are_skipped(self):
        ev = {"gatewayId": "G", "items": [{"rssi": -60, "ts": 1, "adv": telemetry_adv(5), "rsp": identity_rsp()}]}
        self.assertEqual(lam.process(ev, FakeApi({}), MAP)["skipped"], 1)        # no organisation

    def test_device_map_overrides_the_short_id(self):
        ev = {"gatewayId": "G", "items": [{"rssi": -60, "ts": 1, "adv": telemetry_adv(6), "rsp": identity_rsp()}]}
        api = FakeApi({"OTHERID": "org-9"})
        self.assertEqual(lam.process(ev, api, {"15B0B4": "OTHERID"})["posted"], 1)
        self.assertEqual(api.posted[0]["deviceId"], "OTHERID")

    def test_other_ble_traffic_is_ignored(self):
        other = (ad(0x01, b"\x06") + ad(0xFF, struct.pack("<H", 0x004C) + b"\x02\x15" + bytes(21))).hex()
        ev = {"gatewayId": "G", "items": [{"rssi": -50, "ts": 1, "adv": other}, {"rssi": -50, "ts": 1, "adv": "zz"}]}
        self.assertEqual(lam.process(ev, FakeApi(ORGS), MAP), {"readings": 0, "posted": 0, "skipped": 0})

    def test_backend_id_from_the_advert_mac(self):
        self.assertEqual(lam.backend_id("236B2C", "cc:ba:97:23:6b:31"), "CCBA97FFFE236B2C")
        self.assertEqual(lam.backend_id("15B0B4", "50-78-7D-15-B0-B8"), "50787DFFFE15B0B4")
        self.assertEqual(lam.backend_id("15B0B4", None), "15B0B4")    # no MAC: short ID

    def test_device_map_env(self):
        os.environ["DEVICE_MAP"] = '{"15b0b4": "OTHERID"}'
        self.assertEqual(lam.device_map(), {"15B0B4": "OTHERID"})
        os.environ["DEVICE_MAP"] = "not json"
        self.assertEqual(lam.device_map(), {})
        del os.environ["DEVICE_MAP"]


if __name__ == "__main__":
    unittest.main()
