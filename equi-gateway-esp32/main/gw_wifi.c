/*
 * Wi-Fi: Espressif's network_provisioning manager, SoftAP scheme, security 1 (the approach of the
 * SPRO firmware's SoftAP provisioning, commit 903b634), used with the ESP SoftAP Provisioning app.
 * The network is kept by the Wi-Fi driver (its own NVS storage), as the manager does by default.
 * Then a plain station with reconnects, and SNTP (esp_netif_sntp) for the message timestamps.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#include "qrcode.h"
#include "gw.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED  BIT0
#define BIT_PROV_END   BIT1

static EventGroupHandle_t s_ev;
static volatile bool s_provisioning;
static volatile bool s_time_ok;
static int s_retry;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void on_time(struct timeval *tv)
{
    if (!s_time_ok) {
        ESP_LOGI(TAG, "time set by SNTP: %lld", (long long)tv->tv_sec);
    }
    s_time_ok = true;
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
            ESP_LOGI(TAG, "credentials received for \"%s\", connecting", (const char *)c->ssid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            const network_prov_wifi_sta_fail_reason_t *r = data;
            ESP_LOGW(TAG, "connection failed: %s", *r == NETWORK_PROV_WIFI_STA_AUTH_ERROR ?
                     "wrong password" : "network not found");
            network_prov_mgr_reset_wifi_sm_state_on_failure();   /* let the app try again */
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning successful");
            break;
        case NETWORK_PROV_END:
            xEventGroupSetBits(s_ev, BIT_PROV_END);
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (!s_provisioning) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = data;
            xEventGroupClearBits(s_ev, BIT_CONNECTED);
            if (s_provisioning) {
                break;   /* the manager retries and reports to the app */
            }
            int delay_s = s_retry < 5 ? 1 << s_retry : 30;   /* 1, 2, 4, 8, 16, then 30 s */
            s_retry++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry in %d s", d->reason, delay_s);
            gw_led_set(GW_LED_WIFI);
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, delay_s * 1000000LL);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "phone joined the setup network");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

static void run_provisioning(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[20];
    snprintf(name, sizeof(name), "PROV_EQGW_%02X%02X%02X", mac[3], mac[4], mac[5]);
    static const char pop[] = CONFIG_GW_PROV_POP;
    _Static_assert(sizeof(CONFIG_GW_PROV_POP) >= 9, "GW_PROV_POP must be at least 8 characters (Wi-Fi password)");

    ESP_LOGW(TAG, "PROVISIONING: join Wi-Fi \"%s\" (password %s) and open the ESP SoftAP Provisioning app, "
             "PoP %s, or scan this QR code with the app:", name, pop, pop);
    char qr[160];
    snprintf(qr, sizeof(qr), "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\",\"password\":\"%s\"}",
             name, pop, pop);
    ESP_LOGI(TAG, "QR payload: %s", qr);
    esp_qrcode_config_t qcfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&qcfg, qr);

    gw_led_set(GW_LED_PROV);
    s_provisioning = true;
    /* Security 1 (X25519 + AES-CTR) with the PoP; the setup access point uses it as its WPA2 password. */
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, (const void *)pop, name, pop));
    xEventGroupWaitBits(s_ev, BIT_PROV_END, pdFALSE, pdFALSE, portMAX_DELAY);
    s_provisioning = false;
    ESP_LOGI(TAG, "provisioning complete");
}

void gw_wifi_start(void)
{
    s_ev = xEventGroupCreate();
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    network_prov_mgr_config_t pcfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(pcfg));
    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));
    if (!provisioned) {
        run_provisioning();   /* the manager leaves the station connected */
        network_prov_mgr_deinit();
    } else {
        network_prov_mgr_deinit();
        wifi_config_t wc;
        esp_wifi_get_config(WIFI_IF_STA, &wc);
        ESP_LOGI(TAG, "connecting to \"%s\"", (const char *)wc.sta.ssid);
        gw_led_set(GW_LED_WIFI);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    /* Wi-Fi and BLE share the radio: modem sleep is required for coexistence (the default). */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_GW_SNTP_SERVER);
    sc.sync_cb = on_time;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sc));
}

bool gw_wifi_connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & BIT_CONNECTED);
}

bool gw_time_synced(void)
{
    return s_time_ok;
}

int gw_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void gw_wifi_reprovision_and_restart(void)
{
    ESP_LOGW(TAG, "button held: forgetting the Wi-Fi network, restarting into provisioning");
    esp_wifi_restore();   /* clears the driver's stored network; the AWS identity is kept */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

int64_t gw_unix_ms_at(int64_t esp_timer_us)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return now_ms - (esp_timer_get_time() - esp_timer_us) / 1000;
}
