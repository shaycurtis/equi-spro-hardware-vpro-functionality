#pragma once
/*
 * Wi-Fi provisioning ("locked" state), using Espressif's official network_provisioning
 * component over BLE, as the original VPro firmware: the VPro app or Espressif's "ESP BLE
 * Provisioning" app. BLE name "EQUI_<backend id>", security 1, proof of possession
 * CONFIG_VPRO_PROV_BLE_POP ("abc1234", as VPro).
 *
 *   - A device with no saved Wi-Fi network boots into provisioning mode: both LEDs steady
 *     white.
 *   - Credentials received: both LEDs flash white while connecting; success powers the device off
 *     (LEDs dark), a failure goes back to steady white for the app to retry.
 *   - Holding both buttons for 3 s while running re-enters provisioning mode to add a network;
 *     the power button exits (a device that has a saved network then starts normally next time).
 *   - Each successfully provisioned network is kept (up to PROV_MAX_NETWORKS, newest first)
 *     for the Wi-Fi-while-charging work to use.
 */
#include <stdbool.h>
#include <stdint.h>

#define PROV_MAX_NETWORKS   5
#define PROV_TIMEOUT_S      600   /* give up and power off after 10 min in provisioning mode */

/* True if there is no saved network, or re-provisioning was requested. */
bool prov_required(void);

/* Ask for provisioning mode on the next boot and restart now (both-buttons gesture). */
void prov_request_and_restart(void) __attribute__((noreturn));

/* Run provisioning mode, advertising as `ble_name` ("EQUI_<backend id>"). Powers off when done
 * (LEDs dark), on timeout, or on the power button. */
void prov_run(const char *ble_name) __attribute__((noreturn));

/* Saved networks, newest first. Returns false past the end. */
int prov_network_count(void);
bool prov_network_get(int index, char ssid[33], char password[65]);
