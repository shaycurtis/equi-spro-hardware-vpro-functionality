"""
Publish received adverts to AWS IoT Core (MQTT over TLS, X.509 client certificate), in the input
format of the postBleData Lambda (cloud/post_ble_data/post_ble_data.py):

    topic   equimetrics/ble/<gatewayId>/adv
    payload {"gatewayId": ..., "items": [{"mac", "rssi", "ts", "adv" | "rsp": "<hex AD structures>"}]}

One message per device per 60 s burst: every copy of the telemetry advert heard in the burst
(the Lambda keeps the best RSSI of that message) plus the device's latest identity scan response,
so the Lambda can attach the session and horse. Sent BURST_SETTLE_S after the burst's first advert.
postBleData also accepts the advert and the scan response as separate messages, and drops a burst
a second gateway already posted (DynamoDB, see cloud/README.md).

The receiver forwards only our manufacturer-data payload (bytes after the company ID); it is
wrapped back into a manufacturer-data AD structure, so the Lambda parses it exactly like a
commercial gateway's raw advert.

Certificate files (from IoT Core when the thing is created; kept in <repo>/private/iot/, which
git ignores): certificate.pem.crt, private.pem.key, AmazonRootCA1.pem.
"""
from __future__ import annotations

import json
import struct
import threading
import time
from pathlib import Path
from typing import Optional

from equi_protocol import COMPANY_ID, Identity, Telemetry

BURST_SETTLE_S = 3.0          # a burst lasts 2 s
TOPIC_FMT = "equimetrics/ble/{gw}/adv"
DEFAULT_CERT_DIR = Path(__file__).resolve().parent.parent / "private" / "iot"


def wrap_mfg(payload: bytes) -> str:
    """Manufacturer-data AD structure (length, 0xFF, company ID LE, payload) as hex."""
    body = struct.pack("<H", COMPANY_ID) + payload
    return (bytes([1 + len(body), 0xFF]) + body).hex()


class MqttPublisher:
    def __init__(self, gateway_id: str, endpoint: Optional[str] = None, cert_dir: Path = DEFAULT_CERT_DIR,
                 dump_dir: Optional[Path] = None, dry_run: bool = False):
        self.gateway_id = gateway_id
        self.topic = TOPIC_FMT.format(gw=gateway_id)
        self.dump_dir = dump_dir
        self.dry_run = dry_run
        self.lock = threading.Lock()
        self.bursts: dict[tuple, dict] = {}        # (device, tag, seq) -> {"first": t, "items": [...]}
        self.identity: dict[str, dict] = {}        # device -> latest identity item
        self.sent = self.failed = 0
        self.client = None
        if dump_dir:
            dump_dir.mkdir(parents=True, exist_ok=True)
        if not dry_run:
            self._connect(endpoint, cert_dir)
        self._stop = threading.Event()
        threading.Thread(target=self._flusher, daemon=True).start()

    # --------------------------------------------------------------------------- MQTT

    def _connect(self, endpoint: Optional[str], cert_dir: Path) -> None:
        import paho.mqtt.client as mqtt   # pip install paho-mqtt (only needed with --mqtt)

        if not endpoint:
            raise SystemExit("--mqtt needs the IoT Core endpoint (IoT Core -> Settings -> Device data endpoint)")
        files = {n: cert_dir / n for n in ("certificate.pem.crt", "private.pem.key", "AmazonRootCA1.pem")}
        missing = [str(p) for p in files.values() if not p.exists()]
        if missing:
            raise SystemExit("MQTT certificate files missing:\n  " + "\n  ".join(missing))
        # AWS IoT policies usually require the client ID to be the thing name.
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=self.gateway_id)
        c.tls_set(ca_certs=str(files["AmazonRootCA1.pem"]), certfile=str(files["certificate.pem.crt"]),
                  keyfile=str(files["private.pem.key"]))
        c.on_connect = lambda cl, ud, flags, rc, props=None: print(f"MQTT: connected to {endpoint} ({rc})")
        c.on_disconnect = lambda cl, ud, flags, rc, props=None: print(f"MQTT: disconnected ({rc})")
        c.reconnect_delay_set(1, 60)
        c.connect_async(endpoint, 8883, keepalive=60)
        c.loop_start()
        self.client = c

    def close(self) -> None:
        self._stop.set()
        self.flush(force=True)
        if self.client:
            self.client.loop_stop()
            self.client.disconnect()

    # ------------------------------------------------------------------------ batching

    def add(self, address: str, rssi: int, payload: bytes, decoded) -> None:
        """Called by the gateway for every advert / scan response it decoded."""
        item = {"mac": address, "rssi": rssi, "ts": int(time.time() * 1000)}
        with self.lock:
            if isinstance(decoded, Identity):
                item["rsp"] = wrap_mfg(payload)
                self.identity[decoded.device_id] = item
            elif isinstance(decoded, Telemetry):
                item["adv"] = wrap_mfg(payload)
                key = (decoded.device_id, decoded.session_tag, decoded.seq)
                b = self.bursts.setdefault(key, {"first": time.monotonic(), "items": []})
                b["items"].append(item)

    def _flusher(self) -> None:
        while not self._stop.wait(0.5):
            self.flush()

    def flush(self, force: bool = False) -> None:
        now = time.monotonic()
        with self.lock:
            due = [k for k, b in self.bursts.items() if force or now - b["first"] >= BURST_SETTLE_S]
            msgs = []
            for key in due:
                items = self.bursts.pop(key)["items"]
                ident = self.identity.get(key[0])
                if ident:
                    items = items + [ident]
                msgs.append((key, {"gatewayId": self.gateway_id, "items": items}))
        for key, msg in msgs:
            self._send(key, msg)

    def _send(self, key: tuple, msg: dict) -> None:
        body = json.dumps(msg)
        if self.dump_dir:
            (self.dump_dir / f"{key[0]}_{msg['items'][0]['ts']}_seq{key[2]}.json").write_text(json.dumps(msg, indent=2))
        if self.dry_run:
            print(f"MQTT (dry run): {self.topic} {key[0]} seq {key[2]}, {len(msg['items'])} items")
            self.sent += 1
            return
        info = self.client.publish(self.topic, body, qos=1)
        if info.rc == 0:
            self.sent += 1
            print(f"MQTT: published {key[0]} seq {key[2]} ({len(msg['items'])} items)")
        else:
            self.failed += 1
            print(f"MQTT: publish failed ({info.rc}) for {key[0]} seq {key[2]}")
