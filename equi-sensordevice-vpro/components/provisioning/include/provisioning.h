#pragma once
/*
 * Wi-Fi provisioning ("locked" state), using Espressif's official network_provisioning
 * component with the SoftAP scheme and the "ESP SoftAP Provisioning" app (iOS / Android).
 *
 *   - A device with no saved Wi-Fi network boots into provisioning mode: both LEDs steady
 *     white, Wi-Fi access point "PROV_<device id>" (WPA2, password = the device's setup code),
 *     protocomm security 1 with the same setup code as proof of possession.
 *   - Credentials received: both LEDs flash white while connecting; success powers the device off
 *     (LEDs dark), a failure goes back to steady white for the app to retry.
 *   - Holding both buttons for 3 s while running re-enters provisioning mode to add a network;
 *     the power button exits (a device that has a saved network then starts normally next time).
 *   - Each successfully provisioned network is kept (up to PROV_MAX_NETWORKS, newest first)
 *     for the Wi-Fi-while-charging work to use.
 *
 * Setup code: first 8 hex digits of SHA-256(fleet key | "EQ-POP" | device id), so it is
 * unique per device and our backend / tools can compute it (tools/setup_code.py).
 */
#include <stdbool.h>
#include <stdint.h>

#define PROV_MAX_NETWORKS   5
#define PROV_TIMEOUT_S      600   /* give up and power off after 10 min in provisioning mode */

/* True if there is no saved network, or re-provisioning was requested. */
bool prov_required(void);

/* Ask for provisioning mode on the next boot and restart now (both-buttons gesture). */
void prov_request_and_restart(void) __attribute__((noreturn));

/* Run provisioning mode. Powers off when done (LEDs dark), on timeout, or on the power button. */
void prov_run(uint32_t device_id) __attribute__((noreturn));

/* Saved networks, newest first. Returns false past the end. */
int prov_network_count(void);
bool prov_network_get(int index, char ssid[33], char password[65]);
