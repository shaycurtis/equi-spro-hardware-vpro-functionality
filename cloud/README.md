# Cloud (AWS) for the VPro BLE data

Live telemetry from BLE gateways into the Equimetrics backend, the BLE twin of the LoRaWAN path:

```
LoRa: device --LoRaWAN--> IoT Core for LoRaWAN --> rule --> postLoraData --> POST {API_URL}measurements
BLE:  device --advert-->  BLE gateway --MQTT/TLS--> IoT Core --> rule --> postBleData --> same API call
```

`post_ble_data/post_ble_data.py` decodes the 60 s telemetry advert (v5) and the encrypted identity
(session + horse chip) with `gateway/equi_protocol.py`, maps it to the dict `postLoraData` builds,
and posts it through the same `equimetrics_common` layer (`get_org_id`, `post_measurements`). The
input format (raw adverts / scan responses as hex) is described at the top of that file.

Tests (offline, no AWS): `python -m unittest cloud/post_ble_data/test_post_ble_data.py -v`

## Field mapping (BLE -> measurement)

| Measurement | From |
|---|---|
| deviceId | the advert's device ID, e.g. `15B0B4` (optional `DEVICE_MAP` for devices registered under another ID) |
| organisationId | `get_org_id(deviceId)` (unregistered devices are skipped, as LoRa raises) |
| gatewayId, signalStrength | the gateway message; best RSSI of the burst (>= -70 high, >= -85 medium, else low) |
| timestamp | gateway receive time of the burst's first advert (Unix ms) |
| sessionId, horseChipNumber | encrypted identity (session in ms) |
| batteryLevel, temperature, steps | telemetry advert (steps: since power-on) |
| heartRate | telemetry advert; 0 when no beats were counted (never null) |
| horseTemperature, respiration, position, positionsSeen | **temporary placeholders** for the app's charts (`placeholder_horse_metrics`): board temperature, round(heartRate / 3), `"standing"`, `[]` |
| status | Charging / Connected / Disconnected from the USB + charging flags |
| wifiConnected | false (not in the advert) |
| fPort, perspiration, lat/long | not sent (not measured / LoRa only) |

## AWS set-up (dev), in order

1. **Fleet key secret.** Secrets Manager -> Store a new secret -> Other type -> key/value:
   `fleetKey` = the 32 hex digits of `private/fleet_key.bin` (`python -c "print(open('private/fleet_key.bin','rb').read().hex())"`,
   run it yourself; don't paste the output anywhere else). Name: `vpro/dev/ble-fleet-key`.
2. **Build:** `python cloud/post_ble_data/build.py --python <runtime of postLoraData>` -> `dist/`.
3. **Layer:** Lambda -> Layers -> Create layer `cryptography-py<ver>` from `dist/cryptography_layer.zip`
   (compatible runtime = that Python, architecture = that of postLoraData).
4. **Function:** Lambda -> Create function `postBleData`, same runtime/architecture as
   postLoraData, upload `dist/post_ble_data.zip`, handler `post_ble_data.lambda_handler`, timeout
   30 s. Layers: `equimetrics_common`, `equimetrics_env` (same versions as postLoraData), and the
   cryptography layer. Optional environment variables: `FLEET_KEY_SECRET_NAME`,
   `FLEET_KEY_SECRET_PROPERTY`, `POST_WITHOUT_SESSION`, `DEVICE_MAP`.
5. **Permissions:** the function's role needs what postLoraData's has for `nest-lambda-apikey`,
   plus `secretsmanager:GetSecretValue` on `vpro/dev/ble-fleet-key`.
6. **Test in the console** with `post_ble_data/sample_event.json` (one burst from `15B0B4`, made by
   `python cloud/post_ble_data/make_sample_event.py`) and check the measurement in Mongo. The
   device must be registered (the API's devices record with an organisation) or it is skipped
   ("Device not found").
7. **IoT Core**, below.

## IoT Core set-up (gateways -> postBleData)

All in the IoT Core console, region eu-west-1.

**a. Policy (once, shared by every gateway).** Security -> Policies -> Create policy, name
`equimetrics-ble-gateway`, JSON editor (put in your account ID):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:client/${iot:Connection.Thing.ThingName}"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/equimetrics/ble/${iot:Connection.Thing.ThingName}/adv"
    }
  ]
}
```

Each gateway can only connect as itself and publish to its own topic.

**b. Thing + certificate (per gateway).** Manage -> All devices -> Things -> Create things ->
Create single thing, name **`equi-pc-gateway-dev`** (no shadow) -> Auto-generate a new certificate
-> attach the policy `equimetrics-ble-gateway` -> Create thing. **Download all files now** (the
private key is only offered once) into `<repo>/private/iot/` (git-ignored), renamed:

| Downloaded | Save as |
|---|---|
| `…-certificate.pem.crt` | `private/iot/certificate.pem.crt` |
| `…-private.pem.key` | `private/iot/private.pem.key` |
| Amazon Root CA 1 (`AmazonRootCA1.pem`) | `private/iot/AmazonRootCA1.pem` |

(The public key file isn't needed.)

**c. Endpoint.** Settings -> *Device data endpoint*: `xxxxxxxxxxxx-ats.iot.eu-west-1.amazonaws.com`.
Save it as one line in `private/iot/endpoint.txt`; then `--mqtt` needs no argument.

**d. Rule.** Message routing -> Rules -> Create rule, name `equimetrics_ble_adv`, SQL version
2016-03-23:

```sql
SELECT * FROM 'equimetrics/ble/+/adv'
```

Action: *Lambda* -> `postBleData` (the console adds the permission for IoT to invoke it).
Optional error action: CloudWatch logs, to see rule failures.

**e. Run the PC gateway with MQTT** (`pip install -r gateway/requirements.txt` once, for paho-mqtt):

```
python gateway\gateway.py --serial COM4 --web --mqtt
```

It prints `MQTT: connected` and one `MQTT: published 15B0B4 seq N` per 60 s burst. Check: IoT
Core -> MQTT test client -> subscribe to `equimetrics/ble/#` (messages arrive), then the
postBleData logs in CloudWatch (`done: {... 'posted': 1 ...}`) and the measurement in Mongo.
`--mqtt-dry-run` builds the messages without AWS and writes them to `gateway/data/mqtt_out/`.

## Not done yet

- De-duplication across messages / several gateways and an identity cache (DynamoDB, keyed by
  device; needed once more than one gateway hears a device, or a gateway sends adverts and scan
  responses in separate messages).
- A commercial gateway's payload adapter (its JSON -> the input format above).
