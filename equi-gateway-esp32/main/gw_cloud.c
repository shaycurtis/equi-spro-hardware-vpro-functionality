/*
 * AWS IoT Core over esp-mqtt (MQTT 3.1.1, TLS, X.509 client certificate, port 8883).
 *
 * 1. First boot: AWS IoT fleet provisioning by claim (the MQTT API), with the claim certificate
 *    built in from <repo>/private/gateway-claim/:
 *      $aws/certificates/create-from-csr/json             {"certificateSigningRequest": PEM}
 *        -> accepted {certificateId, certificatePem, certificateOwnershipToken}
 *      $aws/provisioning-templates/<template>/provision/json
 *        {"certificateOwnershipToken": ..., "parameters": {"SerialNumber": <base MAC hex>}}
 *        -> accepted {thingName, deviceConfiguration}
 *    The thing name, certificate and key go to NVS (gw_identity).
 * 2. Then: connect as the thing (client ID = thing name, as the shared policy requires) and publish
 *    exactly what gateway/mqtt_publisher.py publishes, for the unchanged postBleData Lambda:
 *      topic   equimetrics/ble/<thing>/adv            (QoS 1)
 *      payload {"gatewayId": <thing>, "items": [{"mac","rssi","ts","adv"}..., {"mac","rssi","ts","rsp"}]}
 *    one message per device per burst; "ts" = receive time in Unix ms.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "gw.h"

static const char *TAG = "cloud";

#ifdef GW_HAVE_CLAIM
extern const char claim_crt_start[] asm("_binary_claim_pem_crt_start");
extern const char claim_key_start[] asm("_binary_claim_private_pem_key_start");
#endif

#define BIT_CONN     BIT0
#define BIT_SUBS     BIT1
#define BIT_RESP     BIT2
#define BIT_DISC     BIT3

static EventGroupHandle_t s_ev;
static esp_mqtt_client_handle_t s_client;
static gw_identity_t s_id;
static volatile bool s_connected;
static uint32_t s_published;
static const char *s_state = "starting";
static char s_topic[192];
static char s_uri[160];

/* Fleet provisioning response (assembled from MQTT_EVENT_DATA chunks). */
static struct {
    char topic[160];
    char *buf;
    int len, cap;
    int subs_pending;
} s_rx;

static const char *endpoint(void)
{
    if (CONFIG_GW_IOT_ENDPOINT[0]) {
        return CONFIG_GW_IOT_ENDPOINT;
    }
#ifdef GW_ENDPOINT_FILE
    return GW_ENDPOINT_FILE;
#else
    return "";
#endif
}

/* ------------------------------------------------------------- MQTT events */

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;
        xEventGroupSetBits(s_ev, BIT_CONN);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "MQTT disconnected");
        }
        s_connected = false;
        xEventGroupClearBits(s_ev, BIT_CONN);
        xEventGroupSetBits(s_ev, BIT_DISC);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (--s_rx.subs_pending <= 0) {
            xEventGroupSetBits(s_ev, BIT_SUBS);
        }
        break;
    case MQTT_EVENT_DATA:
        if (e->current_data_offset == 0) {
            int tl = e->topic_len < (int)sizeof(s_rx.topic) - 1 ? e->topic_len : (int)sizeof(s_rx.topic) - 1;
            memcpy(s_rx.topic, e->topic, tl);
            s_rx.topic[tl] = 0;
            s_rx.len = 0;
            if (e->total_data_len + 1 > s_rx.cap) {
                free(s_rx.buf);
                s_rx.cap = e->total_data_len + 1;
                s_rx.buf = malloc(s_rx.cap);
            }
        }
        if (s_rx.buf && s_rx.len + e->data_len < s_rx.cap) {
            memcpy(s_rx.buf + s_rx.len, e->data, e->data_len);
            s_rx.len += e->data_len;
            s_rx.buf[s_rx.len] = 0;
            if (s_rx.len >= e->total_data_len) {
                xEventGroupSetBits(s_ev, BIT_RESP);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: esp-tls 0x%x, tls stack 0x%x, errno %d",
                     e->error_handle->esp_tls_last_esp_err, e->error_handle->esp_tls_stack_err,
                     e->error_handle->esp_transport_sock_errno);
        } else if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "connection refused (0x%x): certificate / policy / client ID?",
                     e->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static esp_mqtt_client_handle_t make_client(const char *client_id, const char *cert, const char *key, bool reconnect)
{
    esp_mqtt_client_config_t c = {
        .broker.address.uri = s_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,   /* Amazon Root CA 1 */
        .credentials.client_id = client_id,
        .credentials.authentication.certificate = cert,
        .credentials.authentication.key = key,
        .session.keepalive = 60,
        .network.disable_auto_reconnect = !reconnect,
        .network.reconnect_timeout_ms = 10000,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
    };
    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&c);
    if (h) {
        esp_mqtt_client_register_event(h, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    }
    return h;
}

/* --------------------------------------------------- fleet provisioning */

#ifdef GW_HAVE_CLAIM
/* Publish req to topic, wait for <topic>/accepted or /rejected. Returns the parsed accepted body. */
static cJSON *request(esp_mqtt_client_handle_t c, const char *topic, const char *req)
{
    xEventGroupClearBits(s_ev, BIT_RESP);
    if (esp_mqtt_client_publish(c, topic, req, 0, 1, 0) < 0) {
        ESP_LOGE(TAG, "publish to %s failed", topic);
        return NULL;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_RESP | BIT_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(b & BIT_RESP)) {
        ESP_LOGE(TAG, "no answer on %s/accepted|rejected", topic);
        return NULL;
    }
    if (strstr(s_rx.topic, "/rejected")) {
        ESP_LOGE(TAG, "%s rejected: %s", topic, s_rx.buf);
        return NULL;
    }
    return cJSON_Parse(s_rx.buf);
}

static bool fleet_provision(void)
{
    char serial[13], client_id[48], cn[64];
    gw_serial(serial);
    snprintf(client_id, sizeof(client_id), "equi-gw-claim-%s", serial);
    snprintf(cn, sizeof(cn), "%s%s", CONFIG_GW_THING_PREFIX, serial);
    ESP_LOGI(TAG, "fleet provisioning: template \"%s\", serial %s", CONFIG_GW_FLEET_TEMPLATE, cn);

    char *csr = NULL, *key = NULL;
    if (gw_identity_make_csr(cn, &csr, &key) != ESP_OK) {
        return false;
    }
    bool ok = false;
    cJSON *cert_resp = NULL, *prov_resp = NULL;
    char *req = NULL;
    char t_csr[] = "$aws/certificates/create-from-csr/json";
    char t_prov[160];
    snprintf(t_prov, sizeof(t_prov), "$aws/provisioning-templates/%s/provision/json", CONFIG_GW_FLEET_TEMPLATE);

    esp_mqtt_client_handle_t c = make_client(client_id, claim_crt_start, claim_key_start, false);
    xEventGroupClearBits(s_ev, BIT_CONN | BIT_SUBS | BIT_DISC);
    esp_mqtt_client_start(c);
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONN | BIT_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(b & BIT_CONN)) {
        ESP_LOGE(TAG, "claim connection failed (claim certificate active? claim policy attached?)");
        goto out;
    }
    char sub[200];
    s_rx.subs_pending = 4;
    snprintf(sub, sizeof(sub), "%s/accepted", t_csr);
    esp_mqtt_client_subscribe(c, sub, 1);
    snprintf(sub, sizeof(sub), "%s/rejected", t_csr);
    esp_mqtt_client_subscribe(c, sub, 1);
    snprintf(sub, sizeof(sub), "%s/accepted", t_prov);
    esp_mqtt_client_subscribe(c, sub, 1);
    snprintf(sub, sizeof(sub), "%s/rejected", t_prov);
    esp_mqtt_client_subscribe(c, sub, 1);
    if (!(xEventGroupWaitBits(s_ev, BIT_SUBS, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000)) & BIT_SUBS)) {
        ESP_LOGE(TAG, "subscribe failed (claim policy?)");
        goto out;
    }

    /* 1. Certificate from our CSR */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "certificateSigningRequest", csr);
    req = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    cert_resp = request(c, t_csr, req);
    free(req);
    req = NULL;
    const cJSON *pem = cJSON_GetObjectItem(cert_resp, "certificatePem");
    const cJSON *tok = cJSON_GetObjectItem(cert_resp, "certificateOwnershipToken");
    const cJSON *cid = cJSON_GetObjectItem(cert_resp, "certificateId");
    if (!cJSON_IsString(pem) || !cJSON_IsString(tok)) {
        goto out;
    }
    ESP_LOGI(TAG, "certificate %.12s... issued", cJSON_IsString(cid) ? cid->valuestring : "?");

    /* 2. Register the thing through the template */
    r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "certificateOwnershipToken", tok->valuestring);
    cJSON *params = cJSON_AddObjectToObject(r, "parameters");
    cJSON_AddStringToObject(params, "SerialNumber", cn);
    req = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    prov_resp = request(c, t_prov, req);
    const cJSON *thing = cJSON_GetObjectItem(prov_resp, "thingName");
    if (!cJSON_IsString(thing)) {
        goto out;
    }
    ESP_LOGI(TAG, "registered as thing \"%s\"", thing->valuestring);
    ok = gw_identity_save(thing->valuestring, pem->valuestring, key) == ESP_OK;
out:
    esp_mqtt_client_stop(c);
    esp_mqtt_client_destroy(c);
    cJSON_Delete(cert_resp);
    cJSON_Delete(prov_resp);
    free(req);
    free(csr);
    free(key);
    return ok;
}
#endif

/* ---------------------------------------------------------------- publish */

static void hex(char *out, const uint8_t *p, int n)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int build_message(const gw_burst_t *b, char *buf, size_t cap)
{
    char mac[18], h[2 * GW_AD_MAX + 1];
    const uint8_t *a = b->addr;
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    int64_t t0 = gw_unix_ms_at(b->first_rx_us);
    int n = snprintf(buf, cap, "{\"gatewayId\": \"%s\", \"items\": [", s_id.thing);
    hex(h, b->adv, b->adv_len);
    for (int i = 0; i < b->ncopies && n < (int)cap; i++) {
        n += snprintf(buf + n, cap - n, "%s{\"mac\": \"%s\", \"rssi\": %d, \"ts\": %lld, \"adv\": \"%s\"}",
                      i ? ", " : "", mac, b->copies[i].rssi, (long long)(t0 + b->copies[i].dt_ms), h);
    }
    if (b->rsp_len && n < (int)cap) {
        hex(h, b->rsp, b->rsp_len);
        n += snprintf(buf + n, cap - n, ", {\"mac\": \"%s\", \"rssi\": %d, \"ts\": %lld, \"rsp\": \"%s\"}", mac,
                      b->rsp_rssi, (long long)gw_unix_ms_at(b->rsp_rx_us), h);
    }
    if (n < (int)cap) {
        n += snprintf(buf + n, cap - n, "]}");
    }
    return n < (int)cap ? n : -1;
}

static void publish_queued(char *buf, size_t cap)
{
    gw_burst_t b;
    uint32_t qid;
    while (s_connected && gw_queue_peek(&b, &qid)) {
        int n = build_message(&b, buf, cap);
        if (n < 0) {
            ESP_LOGE(TAG, "message too large, dropped");
            gw_queue_pop(qid);
            continue;
        }
        /* QoS 1: esp-mqtt keeps it in its outbox and resends it after a reconnect until PUBACK. */
        int mid = esp_mqtt_client_publish(s_client, s_topic, buf, n, 1, 0);
        if (mid < 0) {
            ESP_LOGW(TAG, "publish failed, keeping %d queued", gw_queue_count());
            return;
        }
        gw_queue_pop(qid);
        if (s_published++ == 0) {
            ESP_LOGI(TAG, "first message: %.*s", n, buf);   /* to compare with the PC gateway's */
        }
        const uint8_t *dev = &b.adv[6];
        ESP_LOGI(TAG, "published %02X%02X%02X (%d items), %d queued", dev[0], dev[1], dev[2],
                 b.ncopies + (b.rsp_len ? 1 : 0), gw_queue_count());
    }
}

static void cloud_task(void *arg)
{
    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:8883", endpoint());
    if (!endpoint()[0]) {
        s_state = "no endpoint";
        ESP_LOGE(TAG, "no IoT Core endpoint: set CONFIG_GW_IOT_ENDPOINT or private/gateway-claim/endpoint.txt");
        gw_led_set(GW_LED_ERROR);
        vTaskDelete(NULL);
    }
    while (!gw_wifi_connected() || !gw_time_synced()) {
        s_state = gw_wifi_connected() ? "waiting for SNTP" : "waiting for Wi-Fi";
        vTaskDelay(pdMS_TO_TICKS(500));
    }

#ifdef GW_HAVE_CLAIM
    int backoff_s = 30;
#endif
    while (!gw_identity_load(&s_id)) {
#ifdef GW_HAVE_CLAIM
        s_state = "fleet provisioning";
        gw_led_set(GW_LED_CLOUD);
        if (gw_wifi_connected() && fleet_provision()) {
            continue;
        }
        ESP_LOGW(TAG, "fleet provisioning failed, retry in %d s", backoff_s);
        s_state = "fleet provisioning failed";
        gw_led_set(GW_LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
        backoff_s = backoff_s < 600 ? backoff_s * 2 : 600;
#else
        s_state = "no identity, no claim certificate built in";
        ESP_LOGE(TAG, "no AWS identity in NVS and no claim certificate in this build (see README)");
        gw_led_set(GW_LED_ERROR);
        vTaskDelay(pdMS_TO_TICKS(60000));
#endif
    }
    snprintf(s_topic, sizeof(s_topic), "equimetrics/ble/%s/adv", s_id.thing);
    ESP_LOGI(TAG, "thing \"%s\", publishing to %s", s_id.thing, s_topic);

    s_client = make_client(s_id.thing, s_id.cert_pem, s_id.key_pem, true);
    esp_mqtt_client_start(s_client);
    s_state = "connecting";
    size_t cap = 160 + (GW_MAX_COPIES + 1) * (64 + 2 * GW_AD_MAX);
    char *buf = malloc(cap);
    for (;;) {
        s_state = s_connected ? "online" : "connecting";
        if (s_connected) {
            publish_queued(buf, cap);   /* batched: everything due, then the radio is BLE's again */
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_GW_PUBLISH_INTERVAL_S * 1000));
    }
}

void gw_cloud_start(void)
{
    s_ev = xEventGroupCreate();
    xTaskCreate(cloud_task, "cloud", 8192, NULL, 5, NULL);
}

bool gw_cloud_connected(void)
{
    return s_connected && s_client;
}

const char *gw_cloud_thing(void)
{
    return s_id.thing[0] ? s_id.thing : "-";
}

uint32_t gw_cloud_published(void)
{
    return s_published;
}

const char *gw_cloud_state(void)
{
    return s_state;
}
