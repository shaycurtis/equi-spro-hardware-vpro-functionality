"""
De-duplication and identity cache for postBleData.

One DynamoDB item per device (partition key `device`, the 6-digit advert id; sort key `sk`):

  identity                         latest session + horse chip
  reading#<session>#<seq>          a burst already claimed for posting
  pending#<tag>#<seq>              a burst still waiting for its identity

A burst is one (session, seq). The sequence byte repeats every 256 minutes, so a later
reading with the same seq is new when its receive time is at least SAME_READING_MS later.
Copies closer than that — another gateway, or a redelivery — are not posted again.

A reading that arrives before any identity for its session tag is kept as pending (24 h)
and posted when the identity shows up, including when the scan response is a separate
message. Until then, a later copy of the same burst can still improve the RSSI and pull
the timestamp earlier. Once a claim is posted, further copies are dropped; the first
claim wins, so we do not wait to see which gateway had the best RSSI.

The claim is conditional. A second writer finds the item and stops. If the writer that
took the claim dies (Lambda timeout is 30 s) the claim goes stale after CLAIM_STALE_S
and another invocation may take it, which covers an IoT retry. Completing the claim
after a successful POST is what makes a later copy a duplicate.

Tests use MemoryStore. Lambda uses DynamoStore (table vpro-dev-ble-dedup, TTL on `ttl`).
"""
from __future__ import annotations

import logging
import os
import time
import uuid

logger = logging.getLogger("postBleData")

REGION = "eu-west-1"
DEFAULT_TABLE = "vpro-dev-ble-dedup"
TABLE_ENV = "BLE_DEDUP_TABLE"

# 200 min is inside the 256 min seq wrap and outside any clock skew we expect.
SAME_READING_MS = 200 * 60 * 1000
CLAIM_STALE_S = 40
# How long a reading waits for its scan response. Long enough for a second message and a
# short offline flush; short enough that a later session which reuses the tag does not
# inherit a pile of readings that never had an identity. DynamoDB TTL is the backstop.
PENDING_HOLD_S = 15 * 60
READING_TTL_S = 24 * 3600
PENDING_TTL_S = 24 * 3600
IDENTITY_TTL_S = 7 * 24 * 3600

IDENTITY_SK = "identity"


def reading_sk(session, seq: int) -> str:
    return f"reading#{session}#{int(seq)}"


def pending_sk(tag: int, seq: int) -> str:
    return f"pending#{int(tag)}#{int(seq)}"


def classify(item: dict, ts: int, now: int) -> str:
    """What an existing reading row means for a new claim: duplicate, steal, or wrap."""
    if abs(int(ts) - int(item["ts"])) < SAME_READING_MS:
        if item.get("claimState") == "posted":
            return "duplicate"
        claimed = int(item.get("claimedAt") or 0)
        if now - claimed >= CLAIM_STALE_S:
            return "steal"
        return "duplicate"
    return "wrap"


def merge_pending(old: dict, new: dict) -> dict:
    """Keep the earliest receive time and the best RSSI across copies of one burst."""
    out = dict(old)
    rssi = new.get("rssi")
    if rssi is not None and (out.get("rssi") is None or rssi > out["rssi"]):
        out["rssi"] = rssi
        if new.get("gatewayId"):
            out["gatewayId"] = new["gatewayId"]
    ts = new.get("ts")
    if ts is not None and (out.get("ts") is None or ts < out["ts"]):
        out["ts"] = ts
    if not out.get("adv") and new.get("adv"):
        out["adv"] = new["adv"]
    if not out.get("mac") and new.get("mac"):
        out["mac"] = new["mac"]
    if out.get("heldAt") is None and new.get("heldAt") is not None:
        out["heldAt"] = new["heldAt"]
    return out


def _failed(exc: BaseException) -> bool:
    response = getattr(exc, "response", None)
    if not isinstance(response, dict):
        return False
    return response.get("Error", {}).get("Code") == "ConditionalCheckFailedException"


def _missing_table(exc: BaseException, table_name: str) -> None:
    response = getattr(exc, "response", None)
    code = response.get("Error", {}).get("Code") if isinstance(response, dict) else None
    if code == "ResourceNotFoundException":
        raise RuntimeError(
            f"DynamoDB table {table_name} does not exist (or this function cannot see it). "
            "Create it and grant access before postBleData runs — cloud/README.md, "
            "\"Still to deploy\". This invocation posted nothing."
        ) from exc
    raise exc


class MemoryStore:
    """In-process stand-in with the same claim rules. One per test, or per process() call."""

    def __init__(self, clock=None):
        self.clock = clock or time.time
        self._identity: dict[str, dict] = {}
        self._readings: dict[tuple, dict] = {}
        self._pending: dict[tuple, dict] = {}

    def now(self) -> int:
        return int(self.clock())

    def get_identity(self, device: str):
        item = self._identity.get(device)
        return None if item is None else dict(item)

    def put_identity(self, device: str, session: int, horse) -> None:
        self._identity[device] = {"session": int(session), "horse": horse or None}

    def claim_reading(self, device: str, session, seq: int, ts: int, gateway: str):
        key = (device, reading_sk(session, seq))
        token = uuid.uuid4().hex
        now = int(self.clock())
        current = self._readings.get(key)
        fresh = {
            "ts": int(ts), "gatewayId": gateway or "", "claimToken": token,
            "claimedAt": now, "claimState": "open",
        }
        if current is None:
            self._readings[key] = fresh
            return token
        kind = classify(current, ts, now)
        if kind == "duplicate":
            return None
        current.update(fresh)
        return token

    def complete_reading(self, device: str, session, seq: int, token: str) -> None:
        item = self._readings.get((device, reading_sk(session, seq)))
        if item and item.get("claimToken") == token:
            item["claimState"] = "posted"

    def release_reading(self, device: str, session, seq: int, token: str) -> None:
        key = (device, reading_sk(session, seq))
        item = self._readings.get(key)
        if item and item.get("claimToken") == token and item.get("claimState") == "open":
            del self._readings[key]

    def put_pending(self, device: str, tag: int, seq: int, record: dict) -> None:
        key = (device, int(tag), int(seq))
        record = dict(record, seq=int(seq), tag=int(tag), heldAt=self.now())
        current = self._pending.get(key)
        self._pending[key] = merge_pending(current, record) if current else record

    def list_pending(self, device: str, tag: int) -> list:
        tag = int(tag)
        return [dict(v) for (dev, t, _seq), v in self._pending.items() if dev == device and t == tag]

    def delete_pending(self, device: str, tag: int, seq: int) -> None:
        self._pending.pop((device, int(tag), int(seq)), None)


class DynamoStore:
    """The same rules on a DynamoDB table. `table` is a boto3 Table resource."""

    def __init__(self, table, table_name: str = DEFAULT_TABLE, clock=None):
        self.table = table
        self.table_name = table_name
        self.clock = clock or time.time

    def now(self) -> int:
        return int(self.clock())

    def _run(self, fn):
        try:
            return fn()
        except Exception as exc:
            if _failed(exc):
                raise
            _missing_table(exc, self.table_name)

    def _get(self, device: str, sk: str):
        resp = self._run(lambda: self.table.get_item(
            Key={"device": device, "sk": sk}, ConsistentRead=True))
        return resp.get("Item")

    def get_identity(self, device: str):
        item = self._get(device, IDENTITY_SK)
        if not item:
            return None
        horse = item.get("horse") or None
        return {"session": int(item["session"]), "horse": horse}

    def put_identity(self, device: str, session: int, horse) -> None:
        now = int(self.clock())
        self._run(lambda: self.table.put_item(Item={
            "device": device, "sk": IDENTITY_SK, "session": int(session),
            "horse": horse or "", "ttl": now + IDENTITY_TTL_S,
        }))

    def claim_reading(self, device: str, session, seq: int, ts: int, gateway: str):
        sk = reading_sk(session, seq)
        token = uuid.uuid4().hex
        now = int(self.clock())

        def fresh_item():
            return {
                "device": device, "sk": sk, "ts": int(ts), "gatewayId": gateway or "",
                "claimToken": token, "claimedAt": now, "claimState": "open",
                "ttl": now + READING_TTL_S,
            }

        try:
            self._run(lambda: self.table.put_item(
                Item=fresh_item(), ConditionExpression="attribute_not_exists(sk)"))
            return token
        except Exception as exc:
            if not _failed(exc):
                raise
        for _ in range(4):
            now = int(self.clock())
            current = self._get(device, sk)
            if current is None:
                try:
                    self._run(lambda: self.table.put_item(
                        Item=fresh_item(), ConditionExpression="attribute_not_exists(sk)"))
                    return token
                except Exception as exc:
                    if not _failed(exc):
                        raise
                    continue
            kind = classify(current, ts, now)
            if kind == "duplicate":
                return None
            values = {
                ":ts": int(ts), ":g": gateway or "", ":tok": token, ":now": now,
                ":open": "open", ":ttl": now + READING_TTL_S,
                ":old": int(current["claimedAt"] if kind == "steal" else current["ts"]),
            }
            cond = ("claimedAt = :old AND claimState = :open" if kind == "steal" else "ts = :old")
            try:
                self._run(lambda: self.table.update_item(
                    Key={"device": device, "sk": sk},
                    ConditionExpression=cond,
                    UpdateExpression=(
                        "SET ts = :ts, gatewayId = :g, claimToken = :tok, claimedAt = :now, "
                        "claimState = :open, #ttl = :ttl"),
                    ExpressionAttributeNames={"#ttl": "ttl"},
                    ExpressionAttributeValues=values,
                ))
                return token
            except Exception as exc:
                if not _failed(exc):
                    raise
        logger.warning(f"dedup claim gave up for {device} {sk}")
        return None

    def complete_reading(self, device: str, session, seq: int, token: str) -> None:
        try:
            self._run(lambda: self.table.update_item(
                Key={"device": device, "sk": reading_sk(session, seq)},
                ConditionExpression="claimToken = :tok",
                UpdateExpression="SET claimState = :posted",
                ExpressionAttributeValues={":tok": token, ":posted": "posted"},
            ))
        except Exception as exc:
            if not _failed(exc):
                raise

    def release_reading(self, device: str, session, seq: int, token: str) -> None:
        try:
            self._run(lambda: self.table.delete_item(
                Key={"device": device, "sk": reading_sk(session, seq)},
                ConditionExpression="claimToken = :tok AND claimState = :open",
                ExpressionAttributeValues={":tok": token, ":open": "open"},
            ))
        except Exception as exc:
            if not _failed(exc):
                raise

    def put_pending(self, device: str, tag: int, seq: int, record: dict) -> None:
        sk = pending_sk(tag, seq)
        current = self._get(device, sk)
        incoming = dict(record, heldAt=self.now())
        merged = merge_pending(_pending_view(current), incoming) if current else incoming
        now = self.now()
        item = {
            "device": device, "sk": sk, "seq": int(seq), "tag": int(tag),
            "gatewayId": merged.get("gatewayId") or "",
            "adv": merged.get("adv") or "", "mac": merged.get("mac") or "",
            "heldAt": int(merged.get("heldAt") or now),
            "ttl": now + PENDING_TTL_S,
        }
        if merged.get("rssi") is not None:
            item["rssi"] = int(merged["rssi"])
        if merged.get("ts") is not None:
            item["ts"] = int(merged["ts"])
        self._run(lambda: self.table.put_item(Item=item))

    def list_pending(self, device: str, tag: int) -> list:
        prefix = f"pending#{int(tag)}#"
        resp = self._run(lambda: self.table.query(
            KeyConditionExpression="device = :d AND begins_with(sk, :p)",
            ExpressionAttributeValues={":d": device, ":p": prefix},
            ConsistentRead=True,
        ))
        return [_pending_view(item) for item in resp.get("Items", [])]

    def delete_pending(self, device: str, tag: int, seq: int) -> None:
        self._run(lambda: self.table.delete_item(Key={"device": device, "sk": pending_sk(tag, seq)}))


def _pending_view(item: dict) -> dict:
    return {
        "seq": int(item["seq"]),
        "tag": int(item["tag"]),
        "mac": item.get("mac") or None,
        "rssi": None if item.get("rssi") is None else int(item["rssi"]),
        "ts": None if "ts" not in item or item["ts"] is None else int(item["ts"]),
        "gatewayId": item.get("gatewayId") or "",
        "adv": item.get("adv") or "",
        "heldAt": None if item.get("heldAt") is None else int(item["heldAt"]),
    }


def dynamo_store_from_env() -> DynamoStore:
    import boto3
    name = os.environ.get(TABLE_ENV, DEFAULT_TABLE)
    table = boto3.resource("dynamodb", region_name=REGION).Table(name)
    return DynamoStore(table, table_name=name)
