/*
 * Equimetrics BLE gateway: shared types and module interfaces.
 *
 *   gw_wifi      SoftAP Wi-Fi provisioning (network_provisioning) + station + SNTP
 *   gw_ble       NimBLE active scan -> one burst per device per 60 s advert burst
 *   gw_queue     RAM ring of finished bursts (buffer while offline, oldest dropped when full)
 *   gw_identity  own key / certificate / thing name in NVS, CSR for fleet provisioning
 *   gw_cloud     esp-mqtt: fleet provisioning by claim, then publish to equimetrics/ble/<thing>/adv
 *   gw_led       dev board RGB LED status
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define GW_AD_MAX      31     /* legacy advert / scan response */
#define GW_MAX_COPIES  24     /* advert copies kept per burst (~18 per 2 s burst) */

typedef struct {
    int8_t rssi;
    uint32_t dt_ms;           /* receive time after the burst's first copy */
} gw_copy_t;

/* One device's 60 s burst: every copy of the telemetry advert + its latest identity scan
 * response. adv / rsp are the manufacturer-data AD structure as on air (len, 0xFF, company, ...). */
typedef struct {
    uint8_t addr[6];          /* BLE address, NimBLE order (LSB first) */
    uint8_t adv_len, rsp_len;
    uint8_t adv[GW_AD_MAX];
    uint8_t rsp[GW_AD_MAX];
    int8_t rsp_rssi;
    int64_t first_rx_us;      /* esp_timer time of the first copy */
    int64_t rsp_rx_us;
    uint8_t ncopies;
    gw_copy_t copies[GW_MAX_COPIES];
} gw_burst_t;

/* ---- gw_queue */
void gw_queue_init(void);
void gw_queue_push(const gw_burst_t *b);          /* drops the oldest when full */
bool gw_queue_peek(gw_burst_t *out, uint32_t *id);
void gw_queue_pop(uint32_t id);
int gw_queue_count(void);
uint32_t gw_queue_dropped(void);

/* ---- gw_ble */
void gw_ble_start(void);
int gw_ble_devices_heard(int within_s);
uint32_t gw_ble_bursts(void);

/* ---- gw_wifi */
void gw_wifi_start(void);                         /* returns once provisioned (station started) */
bool gw_wifi_connected(void);
bool gw_time_synced(void);
int gw_wifi_rssi(void);
void gw_wifi_reprovision_and_restart(void);
int64_t gw_unix_ms_at(int64_t esp_timer_us);      /* esp_timer time -> Unix ms (needs SNTP) */

/* ---- gw_identity */
typedef struct {
    char thing[129];
    char *cert_pem;           /* heap, NUL-terminated */
    char *key_pem;
} gw_identity_t;
bool gw_identity_load(gw_identity_t *id);
esp_err_t gw_identity_save(const char *thing, const char *cert_pem, const char *key_pem);
esp_err_t gw_identity_make_csr(const char *cn, char **csr_pem, char **key_pem);
void gw_identity_free(gw_identity_t *id);
void gw_serial(char out[13]);                     /* base MAC as 12 hex digits */

/* ---- gw_cloud */
void gw_cloud_start(void);
bool gw_cloud_connected(void);
const char *gw_cloud_thing(void);
uint32_t gw_cloud_published(void);
const char *gw_cloud_state(void);

/* ---- gw_led */
typedef enum { GW_LED_BOOT, GW_LED_PROV, GW_LED_WIFI, GW_LED_CLOUD, GW_LED_ONLINE, GW_LED_ERROR, GW_LED_BUTTON } gw_led_state_t;
void gw_led_init(void);
void gw_led_set(gw_led_state_t s);
void gw_led_pulse(void);                          /* short blink: a burst was received */
