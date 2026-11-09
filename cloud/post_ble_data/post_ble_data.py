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

Each 30 s telemetry burst (device, session tag, sequence) is posted once, with the best RSSI and
the first receive time, joined to the encrypted identity (session ID + horse chip) from the same
message. Not done yet (needs DynamoDB once several gateways hear one device): de-duplication
across messages / gateways, and an identity cache across messages.

Environment variables:
    FLEET_KEY_SECRET_NAME      default "vpro/dev/ble-fleet-key"
    FLEET_KEY_SECRET_PROPERTY  default "fleetKey"
    DEVICE_MAP                 optional JSON {"<BLE short ID>": "<backend device ID>"} for devices
                               registered under another ID; by default the short ID (e.g.
                               "15B0B4") is the backend device ID
    POST_WITHOUT_SESSION       "true" to also post readings that have no session yet (default: skip)
"""
import json
import logging
import os
from decimal import Decimal

import equi_protocol as proto

FUNCTION_NAME = "postBleData"
logger = logging.getLogger(FUNCTION_NAME)
logger.setLevel(logging.INFO)

COMPANY_ID = proto.COMPANY_ID        # 0xFFFF test ID (see the firmware's telemetry.h)
AD_TYPE_MFG = 0xFF

_api = None
_fleet_key_loaded = False


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


# ------------------------------------------------------------------------------ decoding

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
    readings = {}     # (device, tag, seq) -> {"t": Telemetry, "rssi", "ts"}
    identities = {}   # device -> list of Identity
    for item in event.get("items", []):
        rssi, ts = item.get("rssi"), item.get("ts")
        for field in ("adv", "rsp"):
            for payload in mfg_payloads(item.get(field, "")):
                d = proto.decode(payload)
                if isinstance(d, proto.Telemetry):
                    key = (d.device_id, d.session_tag, d.seq)
                    r = readings.get(key)
                    if r is None:
                        readings[key] = {"t": d, "rssi": rssi, "ts": ts}
                    else:
                        if rssi is not None and (r["rssi"] is None or rssi > r["rssi"]):
                            r["rssi"] = rssi
                        if ts is not None and (r["ts"] is None or ts < r["ts"]):
                            r["ts"] = ts
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
    if t.heart_rate is not None:
        data["heartRate"] = t.heart_rate
    if t.steps is not None:
        data["steps"] = t.steps                          # since the device was switched on
    return data


# -------------------------------------------------------------------------------- handler

def process(event: dict, api, dev_map: dict, post_without_session: bool = False) -> dict:
    gateway_id = event.get("gatewayId")
    readings, identities = collect(event)
    org_cache = {}
    stats = {"readings": len(readings), "posted": 0, "skipped": 0}
    for (short_id, tag, seq), r in sorted(readings.items(), key=lambda kv: kv[1]["ts"] or 0):
        t = r["t"]
        ident = next((i for i in identities.get(short_id, []) if (i.session & 0xFF) == tag), None)
        if ident is None and not post_without_session:
            logger.info(f"{short_id} seq {seq}: no session yet (tag {tag}), skipped")
            stats["skipped"] += 1
            continue
        device_id = dev_map.get(short_id, short_id)
        if device_id not in org_cache:
            org_cache[device_id] = api.get_org_id(device_id)
        org_id = org_cache[device_id]
        if org_id is None:
            logger.error(f"no organisationId for {device_id}: is it registered?")
            stats["skipped"] += 1
            continue
        data = build_measurement(t, ident, device_id, org_id, gateway_id, r["rssi"], r["ts"])
        logger.info(f"posting {short_id} seq {seq}: {data}")
        if api.post_measurements(data) is not None:
            stats["posted"] += 1
        else:
            stats["skipped"] += 1
    return stats


def lambda_handler(event, context):
    load_fleet_key()
    post_all = os.environ.get("POST_WITHOUT_SESSION", "").lower() == "true"
    stats = process(event, get_api(), device_map(), post_all)
    logger.info(f"done: {stats}")
    return stats
