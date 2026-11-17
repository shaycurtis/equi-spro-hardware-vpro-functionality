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

import ble_dedup                          # noqa: E402
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
                  version=6, horse_temp=None, spare: bytes = b"") -> str:
    """Advert v6 as the firmware's telemetry_encode() builds it (version=5: without position).

    horse_temp=None leaves the 21-byte advert (no probe field). "na" appends the 0x7FFF sentinel.
    A number appends int16 centi-degrees. `spare` is appended after that and must stay unread.
    """
    p = bytes([proto.MAGIC, version]) + DEV + bytes([tag, seq, flags])
    p += struct.pack("<HHhB", round(soc * 256), mv, round(temp * 100), rh) + struct.pack("<I", steps) + bytes([hr])
    if version >= 6:
        b = proto.POSITIONS.index(pos)
        for s in seen:
            b |= 1 << (proto.POSITIONS.index(s) + 2)
        p += bytes([b])
        if horse_temp is not None or spare:
            raw = 0x7FFF if horse_temp in (None, "na") else int(round(float(horse_temp) * 100))
            p += struct.pack("<h", raw)
        p += spare
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


class ConditionalFail(Exception):
    def __init__(self):
        self.response = {"Error": {"Code": "ConditionalCheckFailedException"}}


class FakeTable:
    """Enough of a DynamoDB Table for DynamoStore: conditional put / update / delete and query."""

    def __init__(self):
        self.items = {}

    def get_item(self, Key, ConsistentRead=False):
        item = self.items.get((Key["device"], Key["sk"]))
        return {"Item": dict(item)} if item else {}

    def put_item(self, Item, ConditionExpression=None):
        key = (Item["device"], Item["sk"])
        if ConditionExpression == "attribute_not_exists(sk)" and key in self.items:
            raise ConditionalFail()
        self.items[key] = dict(Item)

    def delete_item(self, Key, ConditionExpression=None, ExpressionAttributeValues=None):
        key = (Key["device"], Key["sk"])
        item = self.items.get(key)
        if ConditionExpression:
            if item is None or not _match(item, ConditionExpression, ExpressionAttributeValues):
                raise ConditionalFail()
        self.items.pop(key, None)

    def update_item(self, Key, ConditionExpression, UpdateExpression, ExpressionAttributeValues,
                    ExpressionAttributeNames=None):
        key = (Key["device"], Key["sk"])
        item = self.items.get(key)
        if item is None or not _match(item, ConditionExpression, ExpressionAttributeValues):
            raise ConditionalFail()
        names = ExpressionAttributeNames or {}
        for part in UpdateExpression.removeprefix("SET ").split(","):
            lhs, rhs = [p.strip() for p in part.split("=")]
            if lhs.startswith("#"):
                lhs = names[lhs]
            item[lhs] = ExpressionAttributeValues[rhs]

    def query(self, KeyConditionExpression, ExpressionAttributeValues, ConsistentRead=False):
        device = ExpressionAttributeValues[":d"]
        prefix = ExpressionAttributeValues[":p"]
        found = [dict(v) for (dev, sk), v in self.items.items() if dev == device and sk.startswith(prefix)]
        return {"Items": found}


def _match(item, expr, values) -> bool:
    for clause in expr.split(" AND "):
        lhs, rhs = [p.strip() for p in clause.split("=")]
        if item.get(lhs) != values[rhs]:
            return False
    return True


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
        self.assertEqual(stats, {"readings": 1, "posted": 1, "skipped": 0, "duplicates": 0, "pending": 0})
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
            m = lam.placeholder_horse_metrics(SimpleNamespace(heart_rate=hr, temp_c=21.5, horse_temp_c=None))
            self.assertEqual((m["heartRate"], m["respiration"]), (0, 0))

    def test_horse_temperature_from_probe(self):
        from decimal import Decimal
        from types import SimpleNamespace
        m = lam.placeholder_horse_metrics(SimpleNamespace(heart_rate=40, temp_c=21.5, horse_temp_c=38.12))
        self.assertEqual(m["horseTemperature"], Decimal("38.12"))       # the probe, not the board
        m = lam.placeholder_horse_metrics(SimpleNamespace(heart_rate=40, temp_c=21.5, horse_temp_c=None))
        self.assertEqual(m["horseTemperature"], Decimal("21.50"))       # no probe: board placeholder

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
        stats = lam.process(ev, api, MAP)
        self.assertEqual((stats["posted"], stats["pending"], stats["skipped"]), (0, 0, 1))
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
        self.assertEqual(lam.process(ev, FakeApi(ORGS), MAP),
                         {"readings": 0, "posted": 0, "skipped": 0, "duplicates": 0, "pending": 0})

    def test_backend_id_from_the_advert_mac(self):
        self.assertEqual(lam.backend_id("236B2C", "cc:ba:97:23:6b:31"), "CCBA97FFFE236B2C")
        self.assertEqual(lam.backend_id("15B0B4", "50-78-7D-15-B0-B8"), "50787DFFFE15B0B4")
        self.assertEqual(lam.backend_id("15B0B4", None), "15B0B4")    # no MAC: short ID

    def test_probe_temperature_is_posted_from_the_advert(self):
        # 23-byte v6, the bytes the firmware appends at offset 21. Board temp stays temperature.
        d = self.post_one(horse_temp=38.12)
        self.assertEqual(d["horseTemperature"], Decimal("38.12"))
        self.assertEqual(d["temperature"], Decimal("22.91"))
        d = self.post_one(horse_temp=0)
        self.assertEqual(d["horseTemperature"], Decimal("0.00"))
        d = self.post_one(horse_temp=-1.25)
        self.assertEqual(d["horseTemperature"], Decimal("-1.25"))

    def test_missing_probe_uses_board_temperature(self):
        d = self.post_one(horse_temp="na")          # 0x7FFF sentinel
        self.assertEqual(d["horseTemperature"], Decimal("22.91"))
        d = self.post_one()                         # 21-byte advert, older v6
        self.assertEqual(d["horseTemperature"], Decimal("22.91"))

    def test_reserved_trailing_byte_is_not_posted(self):
        d = self.post_one(horse_temp=38.12, spare=bytes([0x24]))
        self.assertEqual(d["horseTemperature"], Decimal("38.12"))
        self.assertEqual(d["temperature"], Decimal("22.91"))
        self.assertNotIn("resetReason", d)
        self.assertNotIn("crashCount", d)

    def test_advert_and_scan_response_as_separate_items(self):
        ev = {"gatewayId": "G", "items": [
            {"mac": "50:78:7D:15:B0:B8", "rssi": -70, "ts": 100, "adv": telemetry_adv(4, horse_temp=36.5)},
            {"mac": "50:78:7D:15:B0:B8", "rssi": -64, "ts": 180, "rsp": identity_rsp()},
        ]}
        api = FakeApi(ORGS)
        self.assertEqual(lam.process(ev, api, MAP)["posted"], 1)
        self.assertEqual(api.posted[0]["horseTemperature"], Decimal("36.50"))
        self.assertEqual(api.posted[0]["sessionId"], SESSION)
        self.assertEqual(api.posted[0]["timestamp"], 100)

    def test_scan_response_in_a_later_message(self):
        # The case a gateway (or two gateways) splits: adverts first, identity afterwards.
        store = ble_dedup.MemoryStore()
        api = FakeApi(ORGS)
        mac = "50:78:7D:15:B0:B8"
        adv = lambda rssi, ts: {"gatewayId": "GW-A", "items": [
            {"mac": mac, "rssi": rssi, "ts": ts, "adv": telemetry_adv(9, horse_temp=20.5)}]}
        rsp = {"gatewayId": "GW-B", "items": [
            {"mac": mac, "rssi": -50, "ts": 2000, "rsp": identity_rsp()}]}
        first = lam.process(adv(-80, 1000), api, MAP, store=store)
        self.assertEqual((first["posted"], first["pending"]), (0, 1))
        # A second copy before the identity improves RSSI and keeps the earlier time.
        second = lam.process(adv(-55, 1100), api, MAP, store=store)
        self.assertEqual(second["pending"], 1)
        done = lam.process(rsp, api, MAP, store=store)
        self.assertEqual(done["posted"], 1)
        d = api.posted[0]
        self.assertEqual(d["sessionId"], SESSION)
        self.assertEqual(d["horseId"], HORSE)
        self.assertEqual(d["horseTemperature"], Decimal("20.50"))
        self.assertEqual(d["timestamp"], 1000)
        self.assertEqual(d["signalStrength"], "high")       # -55 from the later copy
        self.assertEqual(d["gatewayId"], "GW-A")

    def test_identity_cached_for_a_later_advert(self):
        store = ble_dedup.MemoryStore()
        api = FakeApi(ORGS)
        mac = "50:78:7D:15:B0:B8"
        rsp = {"gatewayId": "GW-B", "items": [{"mac": mac, "rssi": -50, "ts": 1000, "rsp": identity_rsp()}]}
        adv = {"gatewayId": "GW-A", "items": [
            {"mac": mac, "rssi": -62, "ts": 5000, "adv": telemetry_adv(3, horse_temp=37.25)}]}
        self.assertEqual(lam.process(rsp, api, MAP, store=store)["posted"], 0)
        self.assertEqual(lam.process(adv, api, MAP, store=store)["posted"], 1)
        self.assertEqual(api.posted[0]["horseId"], HORSE)
        self.assertEqual(api.posted[0]["horseTemperature"], Decimal("37.25"))
        self.assertEqual(api.posted[0]["gatewayId"], "GW-A")

    def test_pending_expires_before_a_late_identity(self):
        clock = {"t": 50_000}
        store = ble_dedup.MemoryStore(clock=lambda: clock["t"])
        api = FakeApi(ORGS)
        mac = "50:78:7D:15:B0:B8"
        adv = {"gatewayId": "G", "items": [
            {"mac": mac, "rssi": -60, "ts": 1, "adv": telemetry_adv(9)}]}
        rsp = {"gatewayId": "G", "items": [{"mac": mac, "rssi": -60, "ts": 2, "rsp": identity_rsp()}]}
        self.assertEqual(lam.process(adv, api, MAP, store=store)["pending"], 1)
        clock["t"] += ble_dedup.PENDING_HOLD_S + 1
        late = lam.process(rsp, api, MAP, store=store)
        self.assertEqual(late["posted"], 0)
        self.assertEqual(late["skipped"], 1)
        self.assertEqual(api.posted, [])

    def test_other_session_in_the_cache_does_not_match(self):
        store = ble_dedup.MemoryStore()
        api = FakeApi(ORGS)
        mac = "50:78:7D:15:B0:B8"
        rsp = {"gatewayId": "G", "items": [
            {"mac": mac, "rssi": -50, "ts": 1, "rsp": identity_rsp(session=SESSION + 1)}]}
        adv = {"gatewayId": "G", "items": [{"mac": mac, "rssi": -60, "ts": 2, "adv": telemetry_adv(4)}]}
        lam.process(rsp, api, MAP, store=store)
        stats = lam.process(adv, api, MAP, store=store)
        self.assertEqual(stats["posted"], 0)
        self.assertEqual(stats["pending"], 1)

    def test_second_gateway_is_a_duplicate(self):
        store = ble_dedup.MemoryStore()
        api = FakeApi(ORGS)
        mac = "50:78:7D:15:B0:B8"

        def ev(gw, rssi, ts=5_000):
            return {"gatewayId": gw, "items": [
                {"mac": mac, "rssi": rssi, "ts": ts, "adv": telemetry_adv(8), "rsp": identity_rsp()}]}

        first = lam.process(ev("GW-A", -80), api, MAP, store=store)
        lookups = len(api.lookups)
        second = lam.process(ev("GW-B", -40), api, MAP, store=store)
        self.assertEqual(first["posted"], 1)
        self.assertEqual(second["duplicates"], 1)
        self.assertEqual(second["posted"], 0)
        self.assertEqual(len(api.posted), 1)
        self.assertEqual(api.posted[0]["gatewayId"], "GW-A")   # first claim, not the better RSSI
        self.assertEqual(len(api.lookups), lookups)            # duplicate does not call the API

    def test_sequence_wrap_is_a_new_reading(self):
        # Same seq 256 minutes later is the next time that byte comes around, not a duplicate.
        self.assertLess(ble_dedup.SAME_READING_MS, 256 * 60 * 1000)
        store = ble_dedup.MemoryStore()
        api = FakeApi(ORGS)
        mac = "50:78:7D:15:B0:B8"

        def ev(ts):
            return {"gatewayId": "GW-A", "items": [
                {"mac": mac, "rssi": -60, "ts": ts, "adv": telemetry_adv(8), "rsp": identity_rsp()}]}

        start = 1_700_000_000_000
        self.assertEqual(lam.process(ev(start), api, MAP, store=store)["posted"], 1)
        almost = start + ble_dedup.SAME_READING_MS - 1000
        self.assertEqual(lam.process(ev(almost), api, MAP, store=store)["duplicates"], 1)
        wrapped = start + 256 * 60 * 1000
        self.assertEqual(lam.process(ev(wrapped), api, MAP, store=store)["posted"], 1)
        self.assertEqual(len(api.posted), 2)

    def test_failed_post_can_be_retried(self):
        store = ble_dedup.MemoryStore()
        mac = "50:78:7D:15:B0:B8"
        ev = {"gatewayId": "G", "items": [
            {"mac": mac, "rssi": -60, "ts": 10, "adv": telemetry_adv(2), "rsp": identity_rsp()}]}

        class FailApi(FakeApi):
            def post_measurements(self, data):
                self.posted.append(data)
                return None

        self.assertEqual(lam.process(ev, FailApi(ORGS), MAP, store=store)["skipped"], 1)
        self.assertEqual(lam.process(ev, FakeApi(ORGS), MAP, store=store)["posted"], 1)

    def test_unregistered_device_does_not_keep_the_claim(self):
        store = ble_dedup.MemoryStore()
        mac = "50:78:7D:15:B0:B8"
        ev = {"gatewayId": "G", "items": [
            {"mac": mac, "rssi": -60, "ts": 10, "adv": telemetry_adv(2), "rsp": identity_rsp()}]}
        self.assertEqual(lam.process(ev, FakeApi({}), MAP, store=store)["skipped"], 1)
        api = FakeApi(ORGS)
        self.assertEqual(lam.process(ev, api, MAP, store=store)["posted"], 1)

    def test_stale_claim_can_be_taken_over(self):
        clock = {"t": 1_000}
        store = ble_dedup.MemoryStore(clock=lambda: clock["t"])
        token = store.claim_reading("15B0B4", SESSION, 1, 5_000, "GW-A")
        self.assertIsNotNone(token)
        self.assertIsNone(store.claim_reading("15B0B4", SESSION, 1, 5_000, "GW-B"))
        clock["t"] = 1_000 + ble_dedup.CLAIM_STALE_S
        stolen = store.claim_reading("15B0B4", SESSION, 1, 5_000, "GW-B")
        self.assertIsNotNone(stolen)
        store.complete_reading("15B0B4", SESSION, 1, token)          # the dead owner's token
        self.assertEqual(store._readings[("15B0B4", ble_dedup.reading_sk(SESSION, 1))]["claimState"], "open")
        store.complete_reading("15B0B4", SESSION, 1, stolen)
        self.assertEqual(store._readings[("15B0B4", ble_dedup.reading_sk(SESSION, 1))]["claimState"], "posted")

    def test_dynamo_store_claims_and_pending(self):
        table = FakeTable()
        clock = {"t": 2_000}
        store = ble_dedup.DynamoStore(table, table_name="vpro-dev-ble-dedup", clock=lambda: clock["t"])
        mac = "50:78:7D:15:B0:B8"
        api = FakeApi(ORGS)
        adv = {"gatewayId": "GW-A", "items": [
            {"mac": mac, "rssi": -77, "ts": 8_000, "adv": telemetry_adv(6, horse_temp=36.0)}]}
        better = {"gatewayId": "GW-A", "items": [
            {"mac": mac, "rssi": -60, "ts": 8_100, "adv": telemetry_adv(6, horse_temp=36.0)}]}
        rsp = {"gatewayId": "GW-B", "items": [{"mac": mac, "rssi": -40, "ts": 9_000, "rsp": identity_rsp()}]}
        again = {"gatewayId": "GW-C", "items": [
            {"mac": mac, "rssi": -30, "ts": 8_050, "adv": telemetry_adv(6), "rsp": identity_rsp()}]}
        self.assertEqual(lam.process(adv, api, MAP, store=store)["pending"], 1)
        self.assertEqual(lam.process(better, api, MAP, store=store)["pending"], 1)
        self.assertEqual(lam.process(rsp, api, MAP, store=store)["posted"], 1)
        self.assertEqual(api.posted[0]["signalStrength"], "high")
        self.assertEqual(api.posted[0]["timestamp"], 8_000)
        self.assertEqual(api.posted[0]["horseTemperature"], Decimal("36.00"))
        self.assertEqual(lam.process(again, api, MAP, store=store)["duplicates"], 1)
        self.assertEqual(len(api.posted), 1)
        # A dead claim, then a retry once it is stale.
        clock["t"] = 3_000
        token_key = ("15B0B4", ble_dedup.reading_sk(SESSION, 6))
        table.items[token_key]["claimState"] = "open"
        table.items[token_key]["claimedAt"] = 3_000
        self.assertIsNone(store.claim_reading("15B0B4", SESSION, 6, 8_000, "GW-C"))
        clock["t"] = 3_000 + ble_dedup.CLAIM_STALE_S
        self.assertIsNotNone(store.claim_reading("15B0B4", SESSION, 6, 8_000, "GW-C"))

    def test_device_map_env(self):
        os.environ["DEVICE_MAP"] = '{"15b0b4": "OTHERID"}'
        self.assertEqual(lam.device_map(), {"15B0B4": "OTHERID"})
        os.environ["DEVICE_MAP"] = "not json"
        self.assertEqual(lam.device_map(), {})
        del os.environ["DEVICE_MAP"]


if __name__ == "__main__":
    unittest.main()
