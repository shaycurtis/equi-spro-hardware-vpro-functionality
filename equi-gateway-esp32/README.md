# Equimetrics BLE gateway (ESP32-S3, ESP-IDF 6.1)

A standalone gateway that replaces the PC gateway (`gateway/` + `gateway-receiver/`) on the live data
path. It scans for the SPRO devices' 60 s telemetry adverts and their encrypted identity scan
responses, and publishes them to AWS IoT Core over Wi-Fi in **exactly the PC gateway's message
format**, so the postBleData Lambda runs unchanged:

```
SPRO device --advert + scan response--> ESP32-S3 gateway --Wi-Fi, MQTT/TLS--> IoT Core
    --rule equimetrics_ble_adv--> postBleData --> dev VPro API
```

The gateway **never holds the fleet key**: it forwards the encrypted identity as received, and only
the Lambda decrypts it. It never connects to the devices, so the dev-only "pair by connecting" path
(see the root README, *Gateway receipt*) doesn't apply: a device trusts it only if it already
learned this chip's Bluetooth address. Otherwise the device keeps its `NO_GATEWAY` flag and D1
stays blue. The old receiver kit (`F4:12:FA:E8:6D:12`) is such a case: 15B0B4 and 236B2C learned it
while it was the PC gateway's radio, and that address doesn't change with the firmware, so their D1 goes green. Live ECG over the gateway isn't supported (not needed).

Everything uses official Espressif and AWS mechanisms, unmodified:

| Part | Mechanism |
|---|---|
| Wi-Fi set-up | `espressif/network_provisioning`, SoftAP scheme, security 1, **ESP SoftAP Provisioning** app |
| BLE | NimBLE observer, active scan (from `gateway-receiver/main/receiver_main.c`) |
| Cloud | `espressif/mqtt` (esp-mqtt) over TLS 8883, X.509 client certificate, Amazon Root CA from IDF's certificate bundle |
| Per-gateway identity | **AWS IoT fleet provisioning by claim** (MQTT API, `CreateCertificateFromCsr`), so the private key is made on the gateway and never sent |
| Time | SNTP (`esp_netif_sntp`) |
| LED | `espressif/led_strip` (the dev board's WS2812) |

## Hardware

Any plain ESP32-S3 dev board with 4 MB flash or more, e.g. **ESP32-S3-DevKitC-1** or, better for
range, **-1U** with an external U.FL antenna. No secure boot or flash encryption (dev); never flash
VPro images onto it, or this image onto a SPRO board.

- RGB LED: GPIO 38 (DevKitC-1 **v1.1**, printed "RGB IO38" on the board). **v1.0 boards use
  GPIO 48**: `idf.py menuconfig` -> *Equimetrics gateway* -> *RGB LED GPIO*.
  Brightness: *RGB LED brightness* (`CONFIG_GW_LED_BRIGHTNESS`, default 30 %).
- Button: **BOOT** (GPIO 0).
- Power: any USB port (the "UART" one also gives the log).

## Build and flash

Secrets come from the git-ignored `<repo>/private/` and are built into the image; they never get committed:

| File | What |
|---|---|
| `private/gateway-claim/claim.pem.crt` | fleet provisioning **claim certificate** (AWS set-up, step 3) |
| `private/gateway-claim/claim.private.pem.key` | its private key |
| `private/gateway-claim/endpoint.txt` (optional) | IoT Core endpoint; otherwise the PC gateway's `private/iot/endpoint.txt` is used |

Without the claim files the image still builds (with a CMake warning), but a new gateway can't get
its identity. After adding them, run `idf.py reconfigure`.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd equi-gateway-esp32
idf.py build
idf.py -p COMx flash monitor
```

**WROOM-2 kits (octal flash, e.g. the old receiver kit on COM4)** need the overlay
`sdkconfig.defaults.wroom2` and their own build folder (a quad-flash image doesn't boot on them):

```powershell
idf.py -B build-wroom2 -D SDKCONFIG=build-wroom2/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wroom2" build
idf.py -B build-wroom2 -D SDKCONFIG=build-wroom2/sdkconfig -p COM4 flash monitor
```

`python -m esptool -p COMx flash-id` shows the type (*Flash type set in eFuse: octal*).

**Check the port first.** COM3 is a SPRO board (secure boot) and COM4 the receiver dev kit. Run
`python -m esptool -p COMx --no-stub get-security-info`: it must say *Secure Boot: Disabled*,
and the board must be the gateway board. `idf.py flash` writes a bootloader, which would brick a SPRO board.

`idf.py erase-flash` wipes the Wi-Fi network **and the gateway's AWS identity** (it gets a new
certificate and the same thing name on the next boot; deactivate the old certificate in the console).

## Verified (2026-11-11, WROOM-2 kit `F412FAE86D10`, device 15B0B4)

- SoftAP provisioning with the ESP SoftAP Provisioning app (QR code), network kept across reflashes.
- Fleet provisioning by claim on first boot: certificate issued, thing `equi-gw-F412FAE86D10`
  registered, identity in NVS; later boots go straight to IoT Core.
- Bursts with the identity scan response, published to `equimetrics/ble/equi-gw-F412FAE86D10/adv`.
  The first message, run through postBleData's `process()` locally (fake API), gives device
  `50787DFFFE15B0B4`, its session and horse, and `posted: 1`.
- From boot (Wi-Fi at -72 dBm): Wi-Fi 7 s, SNTP ~34 s, MQTT 35 s; 8-17 advert copies per burst.

## Use

1. **First boot:** the LED blinks **blue slowly**. The log prints a QR code and
   `join Wi-Fi "PROV_EQGW_xxxxxx" (password equigw01)`.
   In the **ESP SoftAP Provisioning** app: *Provision New Device* -> scan the QR code from the log, or *I don't have a QR code* ->
   join `PROV_EQGW_xxxxxx` (password `equigw01`) -> PoP `equigw01` -> pick the barn's network and
   enter its password. A wrong password lets the app try again. The PoP is `CONFIG_GW_PROV_POP`.
2. Then **yellow blinking** while it connects to Wi-Fi, and **cyan blinking** while it waits for SNTP
   and, on the very first boot, **fleet provisioning** (log: `certificate ... issued`,
   `registered as thing "equi-gw-<MAC>"`). The identity stays in NVS, so later boots skip this.
3. **Green, steady:** connected to IoT Core. A short **white flash** for every device burst heard.
   Every burst is logged, and so is every publish:
   `burst 15B0B4: 18 copies, best -52 dBm, with identity` /
   `published 15B0B4 (19 items), 0 queued`.
4. **Red blinking:** error: no claim certificate built in, fleet provisioning rejected (the log
   prints AWS's `errorMessage`), or no endpoint. It retries every 30 s, backing off to 10 min.
5. Every 30 s a status line: Wi-Fi (RSSI), time, cloud state, thing name, **devices heard in the last
   5 minutes**, bursts, published, queued, dropped, free heap.
6. **Change Wi-Fi network:** hold **BOOT** for 3 s while it runs (white flashing). It forgets the
   network and restarts into provisioning. The AWS identity is kept. (Holding BOOT *during reset*
   enters the ROM download mode instead, so press it after boot.)

Offline (Wi-Fi or IoT Core down), bursts queue in RAM, up to 128 (`CONFIG_GW_QUEUE_LEN`, about 280
bytes each). That is about 2 hours for one device, or 13 minutes for 10, and the oldest is dropped
when the queue is full. Timestamps are the real receive times, even for bursts heard before SNTP
had set the clock (they are kept as monotonic time and converted at publish). Published messages
are QoS 1: esp-mqtt resends them after a reconnect until IoT Core acknowledges them. A reboot loses the queue.

Wi-Fi and BLE share the one radio. The scan runs 50 of every 100 ms (`CONFIG_GW_SCAN_WINDOW_MS` /
`_ITVL_MS`; at 90 ms Wi-Fi took ~45 s to connect after boot), and uploads are batched every 5 s (`CONFIG_GW_PUBLISH_INTERVAL_S`).

## Message format (same as `gateway/mqtt_publisher.py`)

- Topic `equimetrics/ble/<thing name>/adv`, QoS 1, client ID = thing name.
- One message per device per burst, sent 3 s after the burst's first advert
  (`CONFIG_GW_BURST_SETTLE_MS`). It holds every copy of the telemetry advert heard (each with its
  own RSSI and receive time), plus the device's latest identity scan response:

```json
{"gatewayId": "equi-gw-3C84279F1A2C", "items": [
  {"mac": "50:78:7D:15:B0:B8", "rssi": -61, "ts": 1794220451123, "adv": "1affffffe9..."},
  ...
  {"mac": "50:78:7D:15:B0:B8", "rssi": -60, "ts": 1794220391020, "rsp": "..ffffffe983..."}]}
```

`adv` / `rsp` hold the manufacturer-data AD structure as received on air (length, `ff`, company ID
`ffff`, magic `e9`, ...), which is exactly what the PC gateway's `wrap_mfg()` rebuilds. Only our
devices are forwarded: company ID 0xFFFF **and** magic 0xE9. The first message after boot is printed
in full (`first message: ...`), so it can be compared with a PC gateway message.

## AWS set-up (one-off, in the console, region eu-west-1)

The existing pieces stay as they are: the policy **`equimetrics-ble-gateway`**, the rule
**`equimetrics_ble_adv`** (`SELECT * FROM 'equimetrics/ble/+/adv'`, which already matches any thing)
and **postBleData**. The PC gateway's thing `equi-pc-gateway-dev` keeps working alongside. The new
parts are the fleet provisioning template, a claim policy and a claim certificate.

**1. Provisioning role.** Let the template wizard (step 4) *Create new role*, named
`equimetrics-iot-fleet-provisioning`. It attaches `AWSIoTThingsRegistration`, which lets IoT Core
create the thing and activate the certificate.

**2. Claim policy.** IoT Core -> Security -> Policies -> Create policy, name
`equimetrics-ble-gateway-claim`, JSON (put in your account ID). It only lets a claim connection ask
for a certificate and run this one template:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:client/equi-gw-claim-*"
    },
    {
      "Effect": "Allow",
      "Action": ["iot:Publish", "iot:Receive"],
      "Resource": [
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/$aws/certificates/create-from-csr/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topic/$aws/provisioning-templates/equimetrics-ble-gateway-fleet/provision/*"
      ]
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": [
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/$aws/certificates/create-from-csr/*",
        "arn:aws:iot:eu-west-1:<ACCOUNT_ID>:topicfilter/$aws/provisioning-templates/equimetrics-ble-gateway-fleet/provision/*"
      ]
    }
  ]
}
```

**3. Claim certificate.** Security -> Certificates -> Add certificate -> Create certificate ->
*Auto-generate*, status **Active** -> Create. Download the certificate and the private key now
(offered only once) into `<repo>/private/gateway-claim/` (git-ignored) as `claim.pem.crt` and
`claim.private.pem.key`. Then open the certificate -> Policies -> **Attach**
`equimetrics-ble-gateway-claim`. Attach no other policy and no thing.

**4. Fleet provisioning template.** Connect many devices -> **Fleet provisioning templates**
(console: *Connect* -> *Connect many devices*) -> Create provisioning template -> *Provisioning
devices with claim certificates*:
- Name **`equimetrics-ble-gateway-fleet`** (`CONFIG_GW_FLEET_TEMPLATE`), status **Active**, role
  `equimetrics-iot-fleet-provisioning`, no pre-provisioning hook (phase 1).
- Claim certificate policy: `equimetrics-ble-gateway-claim`.
- Device permissions: policy **`equimetrics-ble-gateway`** (the existing gateway policy).
- Finish, then open the template -> **Edit JSON** and set its body to this (it names the thing
  `equi-gw-<serial>` from the gateway's `SerialNumber`, the base MAC in hex):

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
        "ThingName": { "Fn::Join": ["", ["equi-gw-", { "Ref": "SerialNumber" }]] },
        "AttributePayload": { "serial": { "Ref": "SerialNumber" } }
      }
    },
    "certificate": {
      "Type": "AWS::IoT::Certificate",
      "Properties": { "CertificateId": { "Ref": "AWS::IoT::Certificate::Id" }, "Status": "ACTIVE" }
    },
    "policy": {
      "Type": "AWS::IoT::Policy",
      "Properties": { "PolicyName": "equimetrics-ble-gateway" }
    }
  }
}
```

**5. Build and flash** (above). On first boot the log shows `certificate ... issued` and
`registered as thing "equi-gw-..."`. In the console the thing appears under *All devices -> Things*,
with its own certificate and `equimetrics-ble-gateway` attached.

**6. Check:** IoT Core -> MQTT test client -> subscribe to `equimetrics/ble/#`. Then the postBleData
CloudWatch logs (`done: {... 'posted': 1 ...}`), and the reading in the app.

The claim certificate is built into every gateway image, but all it allows is fleet provisioning.
If one leaks, deactivate it and make a new one; gateways already provisioned keep their own
certificates. For production, add a **pre-provisioning hook** Lambda that only allows known serials,
and see *Security* below.

## Security notes (phase 1 = dev)

- The gateway's own private key is made on the chip and stored in NVS. On these dev boards NVS is
  **not encrypted** (no flash encryption), and the claim key in the image is readable from flash.
- For production: **flash encryption** in release mode (which also covers the claim key in the
  app image) plus **NVS encryption**. On the S3 the HMAC-based scheme
  (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`) needs no keys partition. Both burn eFuses, which can't be
  undone, so they stay off on the dev boards. `esp_secure_cert` is for certificates written at the
  factory. With fleet provisioning the certificate arrives in the field, so NVS is the right store.
- Provisioning uses security 1 with one PoP for every gateway (as the SPRO did). IDF 6.1 defaults to
  security 2 (SRP6a). Moving to it, with a per-gateway password on a label, is a production item.
- The gateway only publishes to its own topic and can't subscribe to anything. The policy pins the
  client ID and topic to the thing name.

## Files

| File | What |
|---|---|
| `main/gateway_main.c` | start-up, BOOT button, status log + LED state |
| `main/gw_wifi.c` | SoftAP provisioning, station reconnects, SNTP |
| `main/gw_ble.c` | NimBLE active scan, our-device filter, burst grouping |
| `main/gw_queue.c` | offline ring buffer |
| `main/gw_cloud.c` | fleet provisioning by claim, MQTT publish |
| `main/gw_identity.c` | key pair + CSR (PSA / mbedTLS 4), identity in NVS |
| `main/gw_led.c` | RGB LED patterns |
| `main/Kconfig.projbuild` | settings (`idf.py menuconfig` -> *Equimetrics gateway*) |
| `partitions.csv` | 4 MB: NVS 64 KB, two 1.7 MB OTA slots (for phase 3) |

## Later phases (planned, not built)

**Phase 2: settings via the AWS IoT Device Shadow, a dashboard and a local status page.**
- A named shadow `config` per thing. The gateway subscribes to
  `$aws/things/<thing>/shadow/name/config/update/delta`, applies the change, and reports it back
  in `reported`. Settings: active / passive scan, scan interval and window, Coded PHY (needs
  extended scanning, `CONFIG_BT_NIMBLE_EXT_ADV`, and the devices advertising on Coded), company-ID
  and magic filter, minimum RSSI, burst settle time, publish interval and batching (several
  bursts per message would need a matching Lambda change, so it's off by default), log level.
- The gateway policy then also needs `iot:Subscribe` / `iot:Receive` / `iot:Publish` on
  `$aws/things/${iot:Connection.Thing.ThingName}/shadow/name/config/*`.
- A periodic `reported` health block (firmware version, uptime, Wi-Fi RSSI, devices heard,
  queue drops) gives the dashboard its gateway list without a new database.
- Dashboard: a small page (S3 + API Gateway + Lambda, or an extension of the existing web app)
  that lists the things in the template's thing group, shows `reported`, and edits `desired`.
- Local status page: `esp_http_server` on the barn LAN (read-only: state, counters, devices heard,
  last RSSI per device), advertised with mDNS (`espressif/mdns`, `equi-gw-xxxxxx.local`).

**Phase 3: over-the-air updates via AWS IoT Jobs + `esp_https_ota`.**
- Partitions are already laid out for it (two OTA slots). Signed images: Secure Boot v2 on
  production gateways; app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), so a new image
  must reach IoT Core before it marks itself valid.
- The gateway subscribes to `$aws/things/<thing>/jobs/notify-next`. A job document
  `{"operation": "ota", "url": "<pre-signed S3 URL>", "version": "..."}` (AWS IoT Jobs signs the
  S3 URL with its `${aws:iot:s3-presigned-url:...}` placeholder) leads to `esp_https_ota` from
  the URL, job status `IN_PROGRESS` -> `SUCCEEDED` / `FAILED`, then a reboot. Jobs target the
  thing group, with rollouts and abort criteria for staged updates.
- Policy additions: the `$aws/things/<thing>/jobs/*` topics. Plus an IAM role for the pre-signed
  URLs.

**Other follow-ups:** a range test against the PC receiver (-1U board, external antenna); dedup
across several gateways is in postBleData (`cloud/README.md`, DynamoDB) and goes live when that
zip is uploaded; listen-only
trusted-gateway receipts for D1 (the device's trust list from the SD card / Wi-Fi, so these
gateways' scan requests turn it green).
