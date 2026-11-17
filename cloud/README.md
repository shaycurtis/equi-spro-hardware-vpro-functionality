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
| gatewayId, signalStrength | the gateway message; best RSSI of the burst (>= -70 high, >= -85 medium, else low) |
| timestamp | gateway receive time of the burst's first advert (Unix ms) |
| sessionId, horseChipNumber | encrypted identity (session in ms) |
| batteryLevel, temperature, steps | telemetry advert (steps: those of the last 60 s period, as the VPro LoRa device; firmware before v6 sent the total since power-on) |
| heartRate | telemetry advert; 0 when no beats were counted (never null) |
| position, positionsSeen | telemetry advert v6 (`horse_position`): the device's position at the reading (`unknown`, `lateral`, `sternal`, `rolling`, `standing`, `moving`, as the LoRa parser) and the other positions held since the previous reading. Firmware before v6: `"standing"`, `[]` |
| horseTemperature | the external DS18B20 probe (advert v6, 23 bytes); without a probe, or from older firmware: the board temperature (**temporary placeholder**) |
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

**f. ESP32-S3 gateways** (`equi-gateway-esp32/`) publish the same messages to the same topic
pattern under their own things, created on first boot by **fleet provisioning by claim**. The
template, claim policy and claim certificate steps are in `equi-gateway-esp32/README.md` (*AWS set-up*).
The policy, rule and Lambda above are unchanged.

## Not done yet

- De-duplication across messages / several gateways and an identity cache (DynamoDB, keyed by
  device; needed once more than one gateway hears a device, or a gateway sends adverts and scan
  responses in separate messages).
- A commercial gateway's payload adapter (its JSON -> the input format above).

## Sensor identity (AWS IoT for the SPRO sensors themselves)

Phase 1 of `REMOTE_SETTINGS_PLAN.md`: each sensor gets its own AWS IoT thing and certificate, for
settings through the Device Shadow (and later the live log and firmware updates through IoT Jobs).
Firmware: `equi-sensordevice-vpro/components/iot`. It is separate from the gateway's set-up
(`equi-gateway-esp32/README.md`), so a sensor certificate can never publish as a gateway.

**When a device gets its certificate:** automatically, the first time it is on Wi-Fi in
charge-only mode after its Wi-Fi was provisioned. It connects with the **claim certificate** built
into the firmware, sends a CSR for a key it made itself, and stores its own certificate and thing
name in NVS. Nothing is done per device in the console. The steps below are done **once** for the
whole fleet (console, region eu-west-1).

**1. Sensor policy.** IoT Core -> Security -> Policies -> Create policy, name
**`equimetrics-sensor`**, JSON (put in your account ID). A sensor may only connect as its own
thing and use its own shadow, jobs and log topics:

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
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/$aws/things/${iot:Connection.Thing.ThingName}/shadow/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/equimetrics/sensor/${iot:Connection.Thing.ThingName}/*"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/shadow/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/equimetrics/sensor/${iot:Connection.Thing.ThingName}/*"
      ]
    }
  ]
}
```

**2. Claim policy.** Create policy, name **`equimetrics-sensor-claim`**. It only lets a claim
connection ask for a certificate and run the sensor template:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:client/equi-sensor-claim-*"
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/$aws/certificates/create-from-csr/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/$aws/provisioning-templates/equimetrics-sensor-fleet/provision/*"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/$aws/certificates/create-from-csr/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/$aws/provisioning-templates/equimetrics-sensor-fleet/provision/*"
      ]
    }
  ]
}
```

**3. Claim certificate.** Security -> Certificates -> Add certificate -> Create certificate ->
*Auto-generate*, status **Active** -> Create. Download the certificate and the private key now
(offered only once) into `<repo>/private/sensor-claim/` (git-ignored) as `claim.pem.crt` and
`claim.private.pem.key`. Then open the certificate -> Policies -> **Attach**
`equimetrics-sensor-claim`. Attach no other policy and no thing. Do not reuse the gateway's claim
certificate.

**4. Fleet provisioning template.** Connect -> Connect many devices -> Fleet provisioning
templates -> Create provisioning template -> *Provisioning devices with claim certificates*:
- First page ("Describe provisioning template"): status **Active**, name
  **`equimetrics-sensor-fleet`** (it must match the `FLEET_TEMPLATE` constant in the firmware's
  `iot.c`; that is not a console option), role `equimetrics-iot-fleet-provisioning` (the one made
  for the gateway), claim certificate policy `equimetrics-sensor-claim`. Further down the same
  page: tick the **sensor** claim certificate from step 3 (not the gateway's) and leave the
  pre-provisioning action off. Thing settings (name prefix etc.) can be left as offered: the JSON
  below replaces them.
- A later page of the wizard ("Set provisioning permissions"): choose the policy
  `equimetrics-sensor`. This is the policy each provisioned device gets.
- Finish, then open the template -> **Edit JSON** and set its body to this (it names the thing
  `equi-sensor-<backend id>`, e.g. `equi-sensor-DCB4D9FFFE0840E8`):

```json
{
  "Parameters": {
    "SerialNumber": { "Type": "String" },
    "AWS::IoT::Certificate::Id": { "Type": "String" }
  },
  "Resources": {
    "thing": {
      "Type": "AWS::IoT::Thing",
      "OverrideSettings": { "AttributePayload": "MERGE", "ThingTypeName": "REPLACE", "ThingGroups": "DO_NOTHING" },
      "Properties": {
        "ThingName": { "Fn::Join": ["", ["equi-sensor-", { "Ref": "SerialNumber" }]] },
        "AttributePayload": { "serial": { "Ref": "SerialNumber" } }
      }
    },
    "certificate": {
      "Type": "AWS::IoT::Certificate",
      "Properties": { "CertificateId": { "Ref": "AWS::IoT::Certificate::Id" }, "Status": "ACTIVE" }
    },
    "policy": {
      "Type": "AWS::IoT::Policy",
      "Properties": { "PolicyName": "equimetrics-sensor" }
    }
  }
}
```

**5. Build and flash.** In `equi-sensordevice-vpro`: `idf.py reconfigure` (picks up the claim
files; the CMake warning about a missing claim certificate must be gone), `idf.py build`, then the
usual app-only encrypted flash. The endpoint comes from `private/sensor-claim/endpoint.txt` or,
if that is missing, `private/iot/endpoint.txt`.

**6. Check.** Put the device on USB (charge-only). After the charging report the log shows
`iot: NVS: ... free`, `certificate ... issued`, `registered as thing "equi-sensor-..."`, then
`connected to AWS IoT as thing ...`. In the console the thing appears under All devices -> Things
with its certificate and `equimetrics-sensor` attached. Later rounds only show the `connected`
line.

Notes:
- The device's key is stored in NVS, which is **not encrypted** on these boards (same exposure as
  the saved Wi-Fi passwords); the claim key is inside the flash-encrypted app image.
- Provisioning is refused if NVS has fewer than ~100 free entries (the partition is 16 KB), so a
  full NVS cannot leave a certificate in AWS that the device could not store.
- If a device's NVS identity is ever lost it provisions again: the thing keeps its name and gets a
  new certificate; deactivate the old one in the console.

## Firmware updates (AWS IoT Jobs, sensors)

Phase 6 of `REMOTE_SETTINGS_PLAN.md`. A sensor checks for a job at the end of each charging round
(after its shadow), downloads the image, restarts into it, and reports SUCCEEDED once the new
firmware has reached AWS IoT. The sensor policy (`equimetrics-sensor`) already allows the Jobs
topics, so only the pieces below are new. Console, region eu-west-1.

**Versions.** The firmware takes its version from git (`equi-sensordevice-vpro/CMakeLists.txt`):
tag `sensor-v0.2.0` -> `0.2.0`; untagged builds read like `0.1.0-g32e226e` (`-dirty` with
uncommitted changes). The job's `"version"` must equal the image's version exactly, or the device
refuses the image. The device's current version is `reported.fw` in its shadow.

**1. Firmware bucket.** S3 -> Create bucket, e.g. **`equimetrics-sensor-firmware-dev`**, eu-west-1,
*Block all public access* on (the default). Nothing else.

**2. Pre-signing role.** IAM -> Roles -> Create role -> *AWS service* -> **IoT** (use case "IoT").
Name **`equimetrics-iot-jobs-presign`**. Then add an inline policy (put in the bucket name):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": "s3:GetObject",
      "Resource": "arn:aws:s3:::equimetrics-sensor-firmware-dev/sensor/*" }
  ]
}
```

AWS IoT uses this role to make a download link, valid for up to an hour, each time a device
fetches its job; the bucket itself stays private.

**3. Thing groups.** IoT Core -> Manage -> Thing groups -> Create -> *Static*:
**`sensors-bench`** (add `equi-sensor-DCB4D9FFFE0840E8`); later **`sensors-pilot`** (all three
boards) and **`sensors-fleet`**.

**4. A release.**
1. Commit, `git tag sensor-v0.2.0`, build, and check the signature (`espsecure verify-signature`,
   README "Build and flash"). The log line `Firmware version: 0.2.0` comes from cmake.
2. Upload `equi-sensordevice-vpro/build/equi-sensordevice-vpro.bin` to
   `s3://equimetrics-sensor-firmware-dev/sensor/0.2.0/equi-sensordevice-vpro.bin`.
3. Job document (save it as a `.json` file; the console takes it from S3, e.g.
   `sensor/0.2.0/job.json`):

```json
{
  "operation": "ota",
  "version": "0.2.0",
  "url": "${aws:iot:s3-presigned-url:https://equimetrics-sensor-firmware-dev.s3.eu-west-1.amazonaws.com/sensor/0.2.0/equi-sensordevice-vpro.bin}"
}
```

**5. The job.** IoT Core -> Manage -> Remote actions -> Jobs -> Create job -> *Create custom job*:
- Name e.g. `sensor-fw-0-2-0-bench`; targets: thing group `sensors-bench`.
- Job file: the `job.json` above. **Pre-sign resource URLs:** on, role
  `equimetrics-iot-jobs-presign`, link valid 3600 s.
- Job run type: *Snapshot* (only the group's members now) or *Continuous* (also devices added to
  the group later).
- Timeout: in-progress timeout of e.g. 60 min (a device only starts when it is charging; queued
  jobs wait for it). For a group of many: a rollout rate and an abort rule (e.g. abort when more
  than 10 % fail).

**6. Watch it.** The job's *Job executions* tab shows each device: QUEUED until its next charging
round, then IN_PROGRESS (`try` 1..3 in its status details), then SUCCEEDED (or FAILED after 3
tries, or REJECTED for a document it does not understand). On the device: D2 breathes **purple**
during the download; the log shows `job ...: operation ota, version 0.2.0`, `firmware download:
25 % ...`, `restarting into the new firmware`, then after the restart `new firmware 0.2.0
confirmed (reached AWS IoT)` and `job ... -> SUCCEEDED`.

Notes:
- The device refuses a job whose image has a different version than the document says, and the
  bootloader refuses an image not signed with the **spro** key.
- If the new firmware cannot reach AWS IoT, it is not confirmed, and the next restart goes back
  to the previous one; the job then tries again (up to 3 times).
- On a board's first update the image goes to `ota_0`, which overwrites the original SPro app.
