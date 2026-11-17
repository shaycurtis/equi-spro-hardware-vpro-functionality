"""
postBleData: BLE gateway scan data -> Equimetrics measurements (the BLE twin of postLoraData).

    BLE gateway --MQTT/TLS--> AWS IoT Core --rule--> this Lambda --> POST {API_URL}measurements

Uses the same layers as postLoraData:
  - equimetrics_common: EquimetricsApi (get_org_id, post_measurements; API key from Secrets Manager)
  - equimetrics_env:    API_URL, DATA_API_SECRET_NAME, ORG_ID_KEY, ...
plus a layer with the `cryptography` package (AES-CCM for the encrypted identity).

Decoding is gateway/equi_protocol.py, packaged with this file (one decoder for the PC gateway and
the cloud). The fleet key comes from Secrets Manager (FLEET_KEY_SECRET_NAME, a JSON secret whose
FLEET_KEY_SECRET_PROPERTY holds the 16-byte key as 32 hex digits).

Input (IoT rule passes the MQTT JSON through), raw AD structures as they are on air, so any
gateway that forwards raw adverts + scan responses can be adapted to it:
    {
      "gatewayId": "PC-F412FAE86D12",
      "items": [
        {"mac": "50:78:7D:15:B0:B8", "rssi": -61, "ts": 1794220451123,
         "adv": "<hex AD structures of the advert>", "rsp": "<hex of the scan response, optional>"},
        ...
      ]
    }
"ts" is the gateway's receive time in Unix ms. Adverts and scan responses may also come as
separate items (only "adv" or only "rsp").

Each 60 s telemetry burst (device, session, sequence) is posted once. Inside one message the
best RSSI and the first receive time are kept. Across messages and gateways the same burst is
claimed in DynamoDB (ble_dedup.py, keyed by the 6-digit device id): the first claim is posted,
a later copy is not. A reading that arrives before its identity is held for 15 minutes and
posted when the scan response shows up, including when that is a separate message. Tag 0 is
not held: the firmware sends it while there is no session. The sequence byte repeats every
256 minutes, so the same seq that far apart is a new reading.

horseTemperature is the external probe when the advert is 23 bytes, and the board temperature
otherwise. A further advert byte, if one is present, is reserved and is not read. That behaviour
is in this file; the function running in AWS does not have the probe value until the zip is
uploaded (cloud/README.md, "Still to deploy"). This module does not deploy itself.

Environment variables:
    FLEET_KEY_SECRET_NAME      default "vpro/dev/ble-fleet-key"
    FLEET_KEY_SECRET_PROPERTY  default "fleetKey"
    DEVICE_MAP                 optional JSON {"<BLE short ID>": "<backend device ID>"} for devices
                               registered under another ID. By default the backend device ID is
                               the EUI-64 the firmware uses (device_id_backend_get): the advert
                               MAC's OUI + FFFE + the short ID, e.g. 50:78:7D:.. + 15B0B4 ->
                               "50787DFFFE15B0B4" (the VPro app only lists IDs of this form)
    POST_WITHOUT_SESSION       "true" to also post readings that have no session yet (default: skip)
    BLE_DEDUP_TABLE            DynamoDB table for the claim and the identity cache
                               (default "vpro-dev-ble-dedup")
"""
import json
import logging
import os
from decimal import Decimal

import equi_protocol as proto
from ble_dedup import PENDING_HOLD_S, MemoryStore, dynamo_store_from_env

FUNCTION_NAME = "postBleData"
logger = logging.getLogger(FUNCTION_NAME)
logger.setLevel(logging.INFO)

COMPANY_ID = proto.COMPANY_ID        # 0xFFFF test ID (see the firmware's telemetry.h)
AD_TYPE_MFG = 0xFF

_api = None
_fleet_key_loaded = False
_dedup = None


# ------------------------------------------------------------------ set-up (once per container)

def get_api():
    """EquimetricsApi from the equimetrics_common layer (reads the API key on first use)."""
    global _api
    if _api is None:
        from common.equimetrics_api import EquimetricsApi
        _api = EquimetricsApi(logger)
    return _api


def load_fleet_key(secrets_client=None) -> None:
    global _fleet_key_loaded
    if _fleet_key_loaded:
        return
    if secrets_client is None:
        import boto3
        secrets_client = boto3.session.Session().client("secretsmanager", region_name="eu-west-1")
    name = os.environ.get("FLEET_KEY_SECRET_NAME", "vpro/dev/ble-fleet-key")
    prop = os.environ.get("FLEET_KEY_SECRET_PROPERTY", "fleetKey")
    secret = json.loads(secrets_client.get_secret_value(SecretId=name)["SecretString"])
    proto.set_fleet_key(bytes.fromhex(secret[prop]))
    _fleet_key_loaded = True


def device_map() -> dict:
    try:
        return {k.upper(): v for k, v in json.loads(os.environ.get("DEVICE_MAP", "{}")).items()}
    except ValueError:
        logger.error("DEVICE_MAP is not valid JSON")
        return {}


def get_dedup_store():
    """DynamoDB claim store, created once per container. Tests pass their own store to process()."""
    global _dedup
    if _dedup is None:
        _dedup = dynamo_store_from_env()
    return _dedup


# ------------------------------------------------------------------------------ decoding

def backend_id(short_id: str, mac) -> str:
    """EUI-64 device ID from the advert address's OUI and the 6-digit ID in the advert."""
    octets = (mac or "").replace("-", ":").split(":")
    if len(octets) == 6 and all(len(o) == 2 for o in octets[:3]):
        return ("".join(octets[:3]) + "FFFE" + short_id).upper()
    return short_id


def mfg_payloads(hex_ad: str):
    """Yield the bytes after the company ID of each of our manufacturer-data AD structures."""
    if not hex_ad:
        return
    try:
        ad = bytes.fromhex(hex_ad)
    except ValueError:
        return
    i = 0
    while i + 1 < len(ad):
        n = ad[i]
        if n == 0 or i + 1 + n > len(ad):
            break
        if ad[i + 1] == AD_TYPE_MFG and n >= 3 and int.from_bytes(ad[i + 2:i + 4], "little") == COMPANY_ID:
            yield ad[i + 4:i + 1 + n]
        i += 1 + n


def signal_strength(rssi):
    if rssi is None:
        return None
    return "high" if rssi >= -70 else "medium" if rssi >= -85 else "low"


def charger_status(t: proto.Telemetry) -> str:
    """As postLoraData: Charging / Connected (on USB, not charging) / Disconnected."""
    if t.charging:
        return "Charging"
    if t.usb:
        return "Connected"
    return "Disconnected"


def collect(event: dict):
    """Decode a gateway message: unique telemetry readings and the identities seen."""
    readings = {}     # (device, tag, seq) -> {"t": Telemetry, "rssi", "ts", "mac"}
    identities = {}   # device -> list of Identity
    for item in event.get("items", []):
        rssi, ts, mac = item.get("rssi"), item.get("ts"), item.get("mac")
        for field in ("adv", "rsp"):
            for payload in mfg_payloads(item.get(field, "")):
                d = proto.decode(payload)
                if isinstance(d, proto.Telemetry):
                    key = (d.device_id, d.session_tag, d.seq)
                    r = readings.get(key)
                    if r is None:
                        readings[key] = {"t": d, "rssi": rssi, "ts": ts, "mac": mac,
                                         "adv": item.get(field) if field == "adv" else ""}
                    else:
                        if rssi is not None and (r["rssi"] is None or rssi > r["rssi"]):
                            r["rssi"] = rssi
                        if ts is not None and (r["ts"] is None or ts < r["ts"]):
                            r["ts"] = ts
                        r["mac"] = r["mac"] or mac
                elif isinstance(d, proto.Identity):
                    identities.setdefault(d.device_id, [])
                    if d not in identities[d.device_id]:
                        identities[d.device_id].append(d)
    return readings, identities


def build_measurement(t: proto.Telemetry, ident, device_id: str, org_id: str, gateway_id: str,
                      rssi, ts) -> dict:
    """The dict postLoraData hands to api.post_measurements (same keys; LoRa-only ones omitted)."""
    data = {
        "deviceId": device_id,
        "organisationId": org_id,
        "gatewayId": gateway_id,
        "timestamp": ts,
        "signalStrength": signal_strength(rssi),
        "status": charger_status(t),
        "wifiConnected": False,
    }
    if ident is not None:
        data["sessionId"] = ident.session
        if ident.horse_id:
            data["horseId"] = ident.horse_id             # -> horseChipNumber
    if t.battery_soc is not None:
        data["batteryLevel"] = int(round(t.battery_soc))
    if t.temp_c is not None:
        data["temperature"] = Decimal(f"{t.temp_c:.2f}")
    if t.steps is not None:
        data["steps"] = t.steps                          # v6: in the last period (older: since power-on)
    data.update(placeholder_horse_metrics(t))
    data.update(horse_position(t))
    return data


def horse_position(t: proto.Telemetry) -> dict:
    """position + positionsSeen as postLoraData's parser gives them: the position at the reading
    and the other positions held since the last one. Firmware before advert v6 does not send
    a position: "standing" and [] as before, so the app's chart keeps a value."""
    if t.position is None:
        return {"position": "standing", "positionsSeen": []}
    return {"position": t.position,
            "positionsSeen": [p for p in (t.positions_seen or []) if p != t.position]}


# TEMPORARY placeholders so the app's horse charts have values until the SPRO measures these
# itself. Remove each one once the firmware sends the real value.
RESP_PER_HR = 1 / 3     # horse at rest: ~28-44 bpm heart rate, ~8-16 breaths/min


def horse_temperature(t) -> Decimal | None:
    """DS18B20 probe (advert v6, bytes 21-22) when it is present. 0x7FFF and a 21-byte advert
    decode as no probe, and the board temperature stands in so the chart still has a value."""
    celsius = t.horse_temp_c if t.horse_temp_c is not None else t.temp_c
    if celsius is None:
        return None
    return Decimal(f"{celsius:.2f}")


def placeholder_horse_metrics(t: proto.Telemetry) -> dict:
    hr = t.heart_rate or 0                               # no beats counted -> 0, never null
    out = {
        "heartRate": hr,
        "respiration": int(round(hr * RESP_PER_HR)),     # placeholder: derived from heart rate
    }
    horse = horse_temperature(t)
    if horse is not None:
        out["horseTemperature"] = horse
    return out


# -------------------------------------------------------------------------------- handler

def decode_adv(hex_ad: str):
    """First telemetry payload in an advert hex string, or None."""
    for payload in mfg_payloads(hex_ad):
        decoded = proto.decode(payload)
        if isinstance(decoded, proto.Telemetry):
            return decoded
    return None


def _resolve_identity(short_id: str, tag, identities: dict, store):
    """Identity for this session tag: this message first, then the cache from an earlier one."""
    if tag is None:
        return None
    for ident in identities.get(short_id, []):
        if (ident.session & 0xFF) == tag:
            return ident
    cached = store.get_identity(short_id)
    if cached and (int(cached["session"]) & 0xFF) == tag:
        return proto.Identity(short_id, int(cached["session"]), cached.get("horse") or None)
    return None


def _session_key(t: proto.Telemetry, ident):
    """Dedup identity of a burst. The full session once we have it; the tag only when posting
    without one (POST_WITHOUT_SESSION)."""
    if ident is not None:
        return ident.session
    if t.session_tag is None and t.session is not None:
        return t.session
    return f"tag{t.session_tag}"


def process(event: dict, api, dev_map: dict, post_without_session: bool = False, store=None) -> dict:
    """Post one gateway message.

    `store` remembers claims and identities across messages. The Lambda passes DynamoDB;
    a missing store is an empty MemoryStore, so a single call still behaves as one message.
    """
    if store is None:
        store = MemoryStore()
    gateway_id = event.get("gatewayId") or ""
    readings, identities = collect(event)
    for short_id, idents in identities.items():
        latest = idents[-1]
        store.put_identity(short_id, latest.session, latest.horse_id)

    org_cache = {}
    stats = {"readings": len(readings), "posted": 0, "skipped": 0, "duplicates": 0, "pending": 0}

    def deliver(short_id, seq, t, ident, mac, rssi, ts, gateway) -> str:
        session_key = _session_key(t, ident)
        claim_ts = 0 if ts is None else int(ts)
        token = store.claim_reading(short_id, session_key, seq, claim_ts, gateway or "")
        if token is None:
            logger.info(f"{short_id} seq {seq}: duplicate, not posted")
            stats["duplicates"] += 1
            return "duplicate"
        device_id = dev_map.get(short_id) or backend_id(short_id, mac)
        if device_id not in org_cache:
            org_cache[device_id] = api.get_org_id(device_id)
        org_id = org_cache[device_id]
        if org_id is None:
            logger.error(f"no organisationId for {device_id}: is it registered?")
            store.release_reading(short_id, session_key, seq, token)
            stats["skipped"] += 1
            return "skipped"
        data = build_measurement(t, ident, device_id, org_id, gateway or "", rssi, ts)
        logger.info(f"posting {short_id} seq {seq}: {data}")
        if api.post_measurements(data) is not None:
            store.complete_reading(short_id, session_key, seq, token)
            stats["posted"] += 1
            return "posted"
        store.release_reading(short_id, session_key, seq, token)
        stats["skipped"] += 1
        return "skipped"

    for (short_id, tag, seq), r in sorted(readings.items(), key=lambda kv: kv[1]["ts"] or 0):
        t = r["t"]
        ident = _resolve_identity(short_id, tag, identities, store)
        if ident is None and not post_without_session:
            # Tag 0 is what the firmware sends when s_session is 0 (on, but no horse yet).
            # It is also a possible low byte of a real session id, so a reading with this tag
            # is posted when an identity is already known, and dropped — not held — otherwise.
            # Holding it would attach the pre-session backlog to the next tag-0 session.
            if tag is None or tag == 0:
                logger.info(f"{short_id} seq {seq}: no session yet (tag {tag}), skipped")
                stats["skipped"] += 1
                continue
            store.put_pending(short_id, tag, seq, {
                "mac": r.get("mac"), "rssi": r.get("rssi"), "ts": r.get("ts"),
                "gatewayId": gateway_id, "adv": r.get("adv") or "",
            })
            logger.info(f"{short_id} seq {seq}: waiting for identity (tag {tag})")
            stats["pending"] += 1
            continue
        deliver(short_id, seq, t, ident, r.get("mac"), r.get("rssi"), r.get("ts"), gateway_id)

    # Identity in this message can complete a reading an earlier message put aside.
    flushed = set()
    for short_id, idents in identities.items():
        for ident in idents:
            tag = ident.session & 0xFF
            if (short_id, tag) in flushed:
                continue
            flushed.add((short_id, tag))
            now = store.now()
            for pending in store.list_pending(short_id, tag):
                held = pending.get("heldAt")
                if held is not None and now - int(held) > PENDING_HOLD_S:
                    store.delete_pending(short_id, tag, pending["seq"])
                    logger.info(f"{short_id} seq {pending['seq']}: identity arrived after {PENDING_HOLD_S}s, dropped")
                    stats["skipped"] += 1
                    continue
                t = decode_adv(pending.get("adv") or "")
                if t is None:
                    store.delete_pending(short_id, tag, pending["seq"])
                    stats["skipped"] += 1
                    continue
                result = deliver(short_id, t.seq, t, ident, pending.get("mac"), pending.get("rssi"),
                                 pending.get("ts"), pending.get("gatewayId") or gateway_id)
                if result != "skipped":
                    store.delete_pending(short_id, tag, pending["seq"])
    return stats


def lambda_handler(event, context):
    load_fleet_key()
    post_all = os.environ.get("POST_WITHOUT_SESSION", "").lower() == "true"
    stats = process(event, get_api(), device_map(), post_all, store=get_dedup_store())
    logger.info(f"done: {stats}")
    return stats
