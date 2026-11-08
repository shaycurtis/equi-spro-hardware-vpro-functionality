#pragma once
#include <stddef.h>

/*
 * Device identity. Reuses the ID the SPro firmware stored in NVS ("system"/"device_id"),
 * opened READ-ONLY; falls back to the low 3 bytes of the Wi-Fi MAC as 6 hex digits.
 * NVS is never erased here, even if it fails to initialise.
 */
const char *device_id_get(void);
