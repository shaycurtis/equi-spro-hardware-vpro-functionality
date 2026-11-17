# Cloud (AWS) for the VPro BLE data

Live telemetry from BLE gateways into the Equimetrics backend, the BLE twin of the LoRaWAN path:

```
LoRa: device --LoRaWAN--> IoT Core for LoRaWAN --> rule --> postLoraData --> POST {API_URL}measurements
BLE:  device --advert-->  BLE gateway --MQTT/TLS--> IoT Core --> rule --> postBleData --> same API call
```

`post_ble_data/post_ble_data.py` decodes the 60 s telemetry advert (v6; v5 still read) and the encrypted identity
(session + horse chip) with `gateway/equi_protocol.py`, maps it to the dict `postLoraData` builds,
and posts it through the same `equimetrics_common` layer (`get_org_id`, `post_measurements`). The
input format (raw adverts / scan responses as hex) is described at the top of that file.

Tests (offline, no AWS): `python -m unittest cloud/post_ble_data/test_post_ble_data.py -v`

## Field mapping (BLE -> measurement)

| Measurement | From |
|---|---|
| deviceId | EUI-64: the advert MAC's OUI + `FFFE` + the advert's 6-digit ID, e.g. `50787DFFFE15B0B4` (as the firmware's `device_id_backend_get`; optional `DEVICE_MAP` overrides) |
| organisationId | `get_org_id(deviceId)` (unregistered devices are skipped, as LoRa raises) |
| gatewayId, signalStrength | the gateway message; best RSSI inside that message (>= -70 high, >= -85 medium, else low). Another gateway's later copy of the same burst is not posted (see De-duplication) |
| timestamp | gateway receive time of the burst's first advert (Unix ms) |
| sessionId, horseChipNumber | encrypted identity (session in ms) |
| batteryLevel, temperature, steps | telemetry advert (steps: those of the last 60 s period, as the VPro LoRa device; firmware before v6 sent the total since power-on) |
| heartRate | telemetry advert; 0 when no beats were counted (never null) |
| position, positionsSeen | telemetry advert v6 (`horse_position`): the device's position at the reading (`unknown`, `lateral`, `sternal`, `rolling`, `standing`, `moving`, as the LoRa parser) and the other positions held since the previous reading. Firmware before v6: `"standing"`, `[]` |
| horseTemperature | the external DS18B20 probe (advert v6, 23 bytes); without a probe, or from older firmware: the board temperature (temporary placeholder). The function running in AWS still sends that placeholder until the zip in **Still to deploy** is uploaded |
| respiration | **temporary placeholder** for the app's charts (`placeholder_horse_metrics`): round(heartRate / 3) |
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
   `FLEET_KEY_SECRET_PROPERTY`, `POST_WITHOUT_SESSION`, `DEVICE_MAP`, `BLE_DEDUP_TABLE`.
5. **Permissions:** the function's role needs what postLoraData's has for `nest-lambda-apikey`,
   plus `secretsmanager:GetSecretValue` on `vpro/dev/ble-fleet-key`, and the DynamoDB actions in
   **Still to deploy** once that table exists.
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

**f. ESP32-S3 gateways** (`equi-gateway-esp32/`) publish the same messages to the same topic
pattern under their own things, created on first boot by **fleet provisioning by claim**. The
template, claim policy and claim certificate steps are in `equi-gateway-esp32/README.md` (*AWS set-up*).
The policy, rule and Lambda above are unchanged.

## De-duplication

Several gateways can hear one device, and a gateway can publish the advert and the scan response
as two messages. postBleData keeps that in DynamoDB table `vpro-dev-ble-dedup`, one partition per
6-digit device id (partition key `device`, sort key `sk`):

- `identity` — latest session and horse chip, so a reading that arrives in a later message still
  gets them.
- `reading#<session>#<seq>` — a burst already claimed. The first claim is posted, with that
  gateway's RSSI and receive time. Another gateway's copy is not. Inside one message the best
  RSSI and the earliest time are still kept. The sequence byte repeats every 256 minutes, so the
  same seq is a new reading when the two receive times are at least 200 minutes apart.
- `pending#<tag>#<seq>` — a reading that arrived with no identity for its session tag yet. It is
  posted when the identity shows up (same message or a later one, from any gateway), if that is
  within 15 minutes. Until then a later copy can still improve the RSSI and pull the time earlier.

Tag 0 is not held. The firmware sends it when there is no session yet, and a real session id can
also have low byte 0. Holding those readings would attach the pre-session backlog to the next
tag-0 session. A tag-0 reading is posted when its identity is already known.

Rows carry a `ttl` attribute (DynamoDB TTL). The table has to exist before this code runs.

## Still to deploy (AWS console, eu-west-1)

Nothing here was uploaded. The postBleData function running in AWS does not deduplicate across
gateways and does not read the probe temperature. Do these in order. Create the table and grant
access **before** the upload: without the table the new code fails the invocation and that burst
is not posted.

1. **DynamoDB table.** DynamoDB → Create table. Name `vpro-dev-ble-dedup`. Partition key `device`
   (String), sort key `sk` (String). Capacity on-demand. Then the table → Additional settings →
   Time to Live → enable, attribute name `ttl`.
2. **IAM.** On the postBleData execution role, allow these actions on that table (fill in the
   account ID):

```json
{
  "Effect": "Allow",
  "Action": [
    "dynamodb:GetItem",
    "dynamodb:PutItem",
    "dynamodb:UpdateItem",
    "dynamodb:DeleteItem",
    "dynamodb:Query"
  ],
  "Resource": "arn:aws:dynamodb:eu-west-1:<ACCOUNT_ID>:table/vpro-dev-ble-dedup"
}
```

3. **Upload the function zip.** This one upload carries dedup and the probe `horseTemperature`.
   No new function and no new layer. Handler stays `post_ble_data.lambda_handler`. The
   cryptography layer is unchanged.

```
python cloud/post_ble_data/build.py --python <runtime of postLoraData> --no-layer
```

Lambda → postBleData → Code → Upload from → .zip file →
`cloud/post_ble_data/dist/post_ble_data.zip`.

`BLE_DEDUP_TABLE` overrides the table name. Leave it unset to use `vpro-dev-ble-dedup`.

4. **Check.** The committed `sample_event.json` is an older v5 burst. Regenerate it (this reads
   `private/fleet_key.bin` on your machine and writes only the on-air bytes):

```
python cloud/post_ble_data/make_sample_event.py
```

The Lambda test should show `horseTemperature` 37.50 (the probe, not the board's 23.10). Invoke
that same event again: `duplicates` 1, `posted` 0.

`respiration` stays heart rate / 3. A 21-byte advert, or probe sentinel 0x7FFF, still sends the
board temperature as `horseTemperature`. A byte after the probe, if a gateway forwards one, is
reserved and is not posted.

## Not done yet

- A commercial gateway's payload adapter (its JSON -> the input format above).
