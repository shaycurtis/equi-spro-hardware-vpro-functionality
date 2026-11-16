#include "provisioning.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "network_provisioning/manager.h"
#include "sdkconfig.h"
#include "network_provisioning/scheme_ble.h"
#include "power.h"
#include "status_led.h"
#include "telemetry.h"

static const char *TAG = "prov";

#define NVS_NS        "vpro"       /* our own namespace; the SPro "system" data is never touched */
#define KEY_NETWORKS  "wifi_nets"
#define KEY_REPROV    "reprov"

typedef struct {
    char ssid[33];
    char password[65];
} network_t;

static network_t s_nets[PROV_MAX_NETWORKS];
static int s_nnets = -1;            /* -1 = not loaded yet */
static network_t s_pending;         /* credentials received, not yet confirmed */
static volatile bool s_done;

/* ------------------------------------------------------------- storage */

static void load(void)
{
    if (s_nnets >= 0) {
        return;
    }
    s_nnets = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_nets);
    if (nvs_get_blob(h, KEY_NETWORKS, s_nets, &len) == ESP_OK) {
        s_nnets = (int)(len / sizeof(s_nets[0]));
    }
    nvs_close(h);
}

static void save_network(const network_t *n)
{
    load();
    int i = 0;
    while (i < s_nnets && strcmp(s_nets[i].ssid, n->ssid) != 0) {
        i++;
    }
    if (i == s_nnets && s_nnets < PROV_MAX_NETWORKS) {
        s_nnets++;
    }
    for (int k = (i < PROV_MAX_NETWORKS ? i : PROV_MAX_NETWORKS - 1); k > 0; k--) {
        s_nets[k] = s_nets[k - 1];     /* newest first; the oldest drops off when full */
    }
    s_nets[0] = *n;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, KEY_NETWORKS, s_nets, s_nnets * sizeof(s_nets[0]));
        nvs_erase_key(h, KEY_REPROV);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "saved network \"%s\" (%d stored)", n->ssid, s_nnets);
}

int prov_network_count(void)
{
    load();
    return s_nnets;
}

bool prov_network_get(int index, char ssid[33], char password[65])
{
    load();
    if (index < 0 || index >= s_nnets) {
        return false;
    }
    memcpy(ssid, s_nets[index].ssid, 33);
    memcpy(password, s_nets[index].password, 65);
    return true;
}

bool prov_required(void)
{
    uint8_t reprov = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_REPROV, &reprov);
        nvs_close(h);
    }
    return reprov || prov_network_count() == 0;
}

void prov_request_and_restart(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_REPROV, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "provisioning requested: restarting");
    status_led_flash(LED_WHITE, 3, 150, 150);
    /* Not a bare esp_restart(): a session may be running, so recordings are closed and the SD
     * card unmounted first. The power latch survives the software reset. */
    power_restart();
}

/* ----------------------------------------------------------- provisioning */

/* Provisioning mode: both LEDs white, steady (waiting for the app) or flashing (connecting). */
static void show_locked(led_mode_t mode)
{
    status_led_set_mode(LED_IDX_STATUS, mode, LED_WHITE);
    status_led_set_mode(LED_IDX_CHARGE, mode, LED_WHITE);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            const wifi_sta_config_t *c = data;
            memset(&s_pending, 0, sizeof(s_pending));
            memcpy(s_pending.ssid, c->ssid, sizeof(c->ssid));
            memcpy(s_pending.password, c->password, sizeof(c->password));
            ESP_LOGI(TAG, "credentials received for \"%s\", connecting", s_pending.ssid);
            show_locked(LED_MODE_BLINK);   /* flashing white: connecting to Wi-Fi */
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            const network_prov_wifi_sta_fail_reason_t *r = data;
            ESP_LOGW(TAG, "connection failed: %s", *r == NETWORK_PROV_WIFI_STA_AUTH_ERROR ?
                     "wrong password" : "network not found");
            network_prov_mgr_reset_wifi_sm_state_on_failure();   /* let the app try again */
            show_locked(LED_MODE_SOLID);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "connected to \"%s\"", s_pending.ssid);
            save_network(&s_pending);
            break;
        case NETWORK_PROV_END:
            s_done = true;
            break;
        default:
            break;
        }
    }
}

/* Leaving provisioning mode without finishing (power button, timeout): a device that already
 * has a network goes back to normal operation next time instead of re-entering provisioning. */
static void cancel_reprov(void)
{
    if (prov_network_count() == 0) {
        return;   /* still unprovisioned: stays locked */
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_REPROV);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "provisioning cancelled: keeping the %d saved network(s)", s_nnets);
}

static esp_err_t init_default_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;   /* already created */
}

void prov_run(const char *ble_name)
{
    /* Locked / provisioning: both LEDs steady white (same as the SPro production firmware). */
    power_set_charge_led(false);
    show_locked(LED_MODE_SOLID);

    power_register_shutdown_cb(cancel_reprov);

    esp_pm_lock_handle_t lock;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "prov", &lock) == ESP_OK) {
        esp_pm_lock_acquire(lock);   /* radio + provisioning server: stay awake */
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(init_default_loop());
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Same Bluetooth address (and name) as the normal session, so the phone sees one device. */
    telemetry_ble_set_address(NULL);

    /* No BT memory release: the device powers off (or restarts) when provisioning ends. */
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    /* As the original VPro firmware and its app: BLE name "EQUI_<id>", security 1, a fixed proof
     * of possession, the default provisioning service UUID. Must stay valid until NETWORK_PROV_END. */
    static const char pop[] = CONFIG_VPRO_PROV_BLE_POP;
    ESP_LOGI(TAG, "provisioning mode: BLE \"%s\", proof of possession %s", ble_name, pop);
    ESP_LOGI(TAG, "QR payload: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
             ble_name, pop);
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, (const void *)pop,
                                                        ble_name, NULL));

    int64_t deadline = esp_timer_get_time() + (int64_t)PROV_TIMEOUT_S * 1000000;
    while (!s_done && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!s_done) {
        ESP_LOGW(TAG, "provisioning timed out after %d s: powering off", PROV_TIMEOUT_S);
        network_prov_mgr_stop_provisioning();
        power_shutdown();
    }
    network_prov_mgr_deinit();
    esp_wifi_stop();
    /* Done: power off with the LEDs dark (on USB the charge-only LEDs then come on). The next
     * power-on starts a normal session. */
    ESP_LOGI(TAG, "provisioning complete: powering off");
    power_shutdown_silent();
}
