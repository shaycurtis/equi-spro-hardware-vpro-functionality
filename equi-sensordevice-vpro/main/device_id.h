#pragma once
#include <stddef.h>

/*
 * Device identity. Reuses the ID the SPro firmware stored in NVS ("system"/"device_id"),
 * opened READ-ONLY; falls back to the low 3 bytes of the Wi-Fi MAC as 6 hex digits.
 * NVS is never erased here, even if it fails to initialise.
 */
const char *device_id_get(void);

/*
 * The ID the VPro backend and app know the device by: 16 hex digits, an EUI-64 made the standard
 * way from the factory Wi-Fi MAC's OUI + FFFE + the 6-digit ID above, e.g. 50787DFFFE15B0B4
 * (the VPro app only lists devices with IDs of this form). The BLE adverts carry only the 6-digit
 * part; the cloud (postBleData) rebuilds the full ID from the advert's MAC address.
 */
const char *device_id_backend_get(void);
