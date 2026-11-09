/*
 * Charging-time backend calls. See cloud.h.
 */
#include "cloud.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "provisioning.h"
#include "api_env.h"   /* <repo>/private: API_DATA_URL, API_DATA_KEY (not in git) */

static const char *TAG = "cloud";

#define CONNECT_TIMEOUT_MS 20000
#define SNTP_TIMEOUT_MS    15000
#define HTTP_TIMEOUT_MS    20000
#define BIT_GOT_IP         BIT0
#define BIT_FAILED         BIT1

static cloud_cfg_t s_cfg;
static EventGroupHandle_t s_ev;
static char s_ssid[33];

/* ------------------------------------------------------------------ Wi-Fi */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_ev, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static esp_err_t wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* the saved networks live in our own NVS list */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

/* Try the saved networks, newest first. */
static bool wifi_connect(void)
{
    if (esp_wifi_start() != ESP_OK) {
        return false;
    }
    int n = prov_network_count();
    for (int i = 0; i < n; i++) {
        char ssid[33], pass[65];
        if (!prov_network_get(i, ssid, pass)) {
            break;
        }
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
        ESP_LOGI(TAG, "connecting to \"%s\"...", ssid);
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
        if (b & BIT_GOT_IP) {
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
            ESP_LOGI(TAG, "connected to \"%s\"", ssid);
            return true;
        }
        ESP_LOGW(TAG, "\"%s\": %s", ssid, (b & BIT_FAILED) ? "failed" : "timed out");
        esp_wifi_disconnect();
    }
    esp_wifi_stop();
    return false;
}

static void wifi_off(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}

/* ------------------------------------------------------------------- time */

static bool time_sync(void)
{
    static bool started;
    if (!started) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&c);
        started = true;
    } else {
        esp_netif_sntp_start();
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP: no answer; using the RTC time");
        return false;
    }
    time_t now = time(NULL);
    ESP_LOGI(TAG, "time synced: %lld", (long long)now);
    if (s_cfg.set_time) {
        s_cfg.set_time((uint32_t)now);
    }
    return true;
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------- HTTPS */

/* POST JSON to API_DATA_URL + path with the data key. Returns the HTTP status (< 0: no reply). */
static int post_json(const char *path, const char *body)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/%s", API_DATA_URL, path);
    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* verify the server certificate */
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return -1;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "x-api-key", API_DATA_KEY);
    int status = -1;
    size_t len = strlen(body);
    esp_err_t err = esp_http_client_open(c, len);
    if (err == ESP_OK && esp_http_client_write(c, body, len) == (int)len) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char resp[257] = { 0 };
        int n = esp_http_client_read_response(c, resp, sizeof(resp) - 1);
        ESP_LOGI(TAG, "POST /%s -> %d %s", path, status, n > 0 ? resp : "");
    } else {
        ESP_LOGW(TAG, "POST /%s failed: %s", path, esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    return status;
}

/* ------------------------------------------------------- charging telemetry */

static int send_charging(const cloud_status_t *st)
{
    int64_t t = now_ms();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "deviceId", s_cfg.device_id);
    cJSON_AddNumberToObject(j, "sessionId", (double)t);   /* each report its own ID: send time */
    cJSON_AddNumberToObject(j, "timeStamp", (double)t);
    cJSON *d = cJSON_AddObjectToObject(j, "device");
    cJSON_AddStringToObject(d, "hwVersion", s_cfg.hw_version);
    cJSON_AddStringToObject(d, "fwVersion", s_cfg.fw_version);
    cJSON_AddStringToObject(d, "lastUpdate", s_cfg.last_update);
    cJSON_AddNumberToObject(d, "batteryLevel", st->battery_pct);
    char tc[16];
    snprintf(tc, sizeof(tc), "%.2f", st->temperature_c);
    cJSON_AddRawToObject(d, "temperature", tc);   /* 2 decimals, as VPro */
    cJSON_AddNumberToObject(d, "humidity", (int)(st->humidity_pct + 0.5f));
    cJSON_AddStringToObject(d, "chargerStatus", st->charging ? "Charging" : "ChargingComplete");
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "ssid", s_ssid);
    cJSON_AddArrayToObject(w, "fileProgress");
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return -1;
    }
    ESP_LOGI(TAG, "charging telemetry: %s", body);
    int status = post_json("api/sessions/events/charging", body);
    cJSON_free(body);
    return status;
}

/* ------------------------------------------------------------------- task */

static void cloud_task(void *arg)
{
    bool sent_full = false;
    if (strstr(API_DATA_KEY, "PASTE_") || !API_DATA_URL[0]) {
        ESP_LOGE(TAG, "private/api_env.h has no data key: cloud calls disabled");
        vTaskDelete(NULL);
    }
    if (prov_network_count() == 0) {
        ESP_LOGW(TAG, "no saved Wi-Fi network: cloud calls disabled");
        vTaskDelete(NULL);
    }
    if (wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed");
        vTaskDelete(NULL);
    }
    for (;;) {
        cloud_status_t st = { .charging = true };
        if (s_cfg.read_status) {
            s_cfg.read_status(&st);
        }
        if (!st.charging && sent_full) {
            ESP_LOGI(TAG, "battery full and reported: cloud task done");
            break;
        }
        if (wifi_connect()) {
            time_sync();
            if (time(NULL) < 1704067200) {   /* before 2024: certificates can't be checked */
                ESP_LOGW(TAG, "no valid time: skipping HTTPS");
            } else {
                int status = send_charging(&st);
                if (status / 100 == 2 && !st.charging) {
                    sent_full = true;
                }
            }
            wifi_off();
        } else {
            ESP_LOGW(TAG, "no saved network reachable");
        }
        vTaskDelay(pdMS_TO_TICKS(CLOUD_PERIOD_S * 1000));
    }
    vTaskDelete(NULL);
}

esp_err_t cloud_charging_start(const cloud_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_ev = xEventGroupCreate();
    if (!s_ev) {
        return ESP_ERR_NO_MEM;
    }
    /* TLS needs a fair stack; PSRAM is fine for it. */
    return xTaskCreate(cloud_task, "cloud", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
