/*
 * AWS IoT Core connection for the sensor. See iot.h. Adapted from the gateway's gw_identity.c /
 * gw_cloud.c (same fleet provisioning flow over esp-mqtt).
 */
#include "iot.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "iot";

#define NVS_NS          "iot"          /* our own namespace */
#define THING_PREFIX    "equi-sensor-"
#define FLEET_TEMPLATE  "equimetrics-sensor-fleet"
#define WAIT_MS         30000

/* NVS room needed for the identity: a blob takes one entry (32 bytes) per 32 bytes of data plus
 * two of overhead per chunk; certificate ~1.3 KB, key ~0.25 KB, thing name. Checked BEFORE fleet
 * provisioning, because every attempt makes a new certificate in AWS. The margin leaves room for
 * the other users of this small (16 KB) partition: BLE bonds, Wi-Fi networks, gateways. */
#define NVS_ENTRIES_NEEDED  64
#define NVS_ENTRIES_MARGIN  40

#ifdef IOT_HAVE_CLAIM
extern const char claim_crt_start[] asm("_binary_claim_pem_crt_start");
extern const char claim_key_start[] asm("_binary_claim_private_pem_key_start");
#endif

#define BIT_CONN  BIT0
#define BIT_SUBS  BIT1
#define BIT_RESP  BIT2
#define BIT_DISC  BIT3

typedef struct {
    char thing[64];
    char *cert_pem;   /* heap, NUL-terminated */
    char *key_pem;
} identity_t;

static EventGroupHandle_t s_ev;
static char s_uri[160];

/* The response being received (assembled from MQTT_EVENT_DATA chunks). */
static struct {
    char topic[160];
    char *buf;
    int len, cap;
    int subs_pending;
} s_rx;

/* ---------------------------------------------------------------- identity */

static char *get_blob_str(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) != ESP_OK || len == 0) {
        return NULL;
    }
    char *s = malloc(len + 1);
    if (s && nvs_get_blob(h, key, s, &len) != ESP_OK) {
        free(s);
        return NULL;
    }
    if (s) {
        s[len] = 0;
    }
    return s;
}

static void identity_free(identity_t *id)
{
    free(id->cert_pem);
    free(id->key_pem);
    id->cert_pem = id->key_pem = NULL;
}

static bool identity_load(identity_t *id)
{
    memset(id, 0, sizeof(*id));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t tl = sizeof(id->thing);
    bool ok = nvs_get_str(h, "thing", id->thing, &tl) == ESP_OK;   /* written last: complete */
    if (ok) {
        id->cert_pem = get_blob_str(h, "cert");
        id->key_pem = get_blob_str(h, "key");
    }
    nvs_close(h);
    if (!ok || !id->cert_pem || !id->key_pem) {
        identity_free(id);
        return false;
    }
    return true;
}

#ifdef IOT_HAVE_CLAIM
/* Blobs (not strings): a blob may span NVS pages, a string must fit in the free part of one. */
static esp_err_t identity_save(const char *thing, const char *cert_pem, const char *key_pem)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "cert", cert_pem, strlen(cert_pem));
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "key", key_pem, strlen(key_pem));
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "thing", thing);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        nvs_erase_key(h, "thing");   /* leave nothing half-written (only our own keys) */
        nvs_erase_key(h, "cert");
        nvs_erase_key(h, "key");
        nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "saved identity \"%s\": %s", thing, esp_err_to_name(err));
    return err;
}
#endif

/* Free NVS entries (one entry = 32 bytes), logged every round; -1 if unknown. */
static int nvs_free_entries(void)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return -1;
    }
    ESP_LOGI(TAG, "NVS: %u of %u entries used, %u free, %u namespaces", (unsigned)st.used_entries,
             (unsigned)st.total_entries, (unsigned)st.free_entries, (unsigned)st.namespace_count);
    return (int)st.free_entries;
}

#ifdef IOT_HAVE_CLAIM
/* New P-256 key pair and a CSR for it (PEM, heap). Only the CSR leaves the device. */
static esp_err_t make_csr(const char *cn, char **csr_pem, char **key_pem)
{
    *csr_pem = *key_pem = NULL;
    esp_err_t result = ESP_FAIL;
    psa_key_id_t kid = 0;
    mbedtls_pk_context pk;
    mbedtls_x509write_csr csr;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr_init(&csr);
    char *csr_buf = calloc(1, 1024), *key_buf = calloc(1, 512);
    char subject[160];
    int rc;

    if (!csr_buf || !key_buf || psa_crypto_init() != PSA_SUCCESS) {
        goto out;
    }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_status_t st = psa_generate_key(&a, &kid);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key %d", (int)st);
        goto out;
    }
    if ((rc = mbedtls_pk_copy_from_psa(kid, &pk)) != 0) {
        ESP_LOGE(TAG, "pk_copy_from_psa -0x%04x", -rc);
        goto out;
    }
    snprintf(subject, sizeof(subject), "CN=%s,O=Equimetrics", cn);
    mbedtls_x509write_csr_set_key(&csr, &pk);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    if ((rc = mbedtls_x509write_csr_set_subject_name(&csr, subject)) != 0 ||
        (rc = mbedtls_x509write_csr_pem(&csr, (unsigned char *)csr_buf, 1024)) != 0 ||
        (rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)key_buf, 512)) != 0) {
        ESP_LOGE(TAG, "CSR / key PEM -0x%04x", -rc);
        goto out;
    }
    *csr_pem = csr_buf;
    *key_pem = key_buf;
    csr_buf = key_buf = NULL;
    result = ESP_OK;
out:
    if (kid) {
        psa_destroy_key(kid);
    }
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    free(csr_buf);
    free(key_buf);
    return result;
}
#endif

/* -------------------------------------------------------------------- MQTT */

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_ev, BIT_CONN);
        break;
    case MQTT_EVENT_DISCONNECTED:
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
                if (!s_rx.buf) {
                    s_rx.cap = 0;
                }
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

/* One connection per round: no automatic reconnect (the caller's next round tries again). */
static esp_mqtt_client_handle_t client_start(const char *client_id, const char *cert, const char *key)
{
    esp_mqtt_client_config_t c = {
        .broker.address.uri = s_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,   /* Amazon Root CA 1 */
        .credentials.client_id = client_id,
        .credentials.authentication.certificate = cert,
        .credentials.authentication.key = key,
        .session.keepalive = 60,
        .network.disable_auto_reconnect = true,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
    };
    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&c);
    if (!h) {
        return NULL;
    }
    esp_mqtt_client_register_event(h, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    xEventGroupClearBits(s_ev, BIT_CONN | BIT_SUBS | BIT_RESP | BIT_DISC);
    if (esp_mqtt_client_start(h) != ESP_OK) {
        esp_mqtt_client_destroy(h);
        return NULL;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONN | BIT_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(WAIT_MS));
    if (!(b & BIT_CONN)) {
        esp_mqtt_client_stop(h);
        esp_mqtt_client_destroy(h);
        return NULL;
    }
    return h;
}

static void client_stop(esp_mqtt_client_handle_t h)
{
    esp_mqtt_client_disconnect(h);
    esp_mqtt_client_stop(h);
    esp_mqtt_client_destroy(h);
    free(s_rx.buf);
    s_rx.buf = NULL;
    s_rx.cap = 0;
}

/* Subscribe to <topic>/accepted and <topic>/rejected for each topic; true once all are acknowledged. */
static bool subscribe_responses(esp_mqtt_client_handle_t c, const char *const *topics, int n)
{
    char sub[200];
    xEventGroupClearBits(s_ev, BIT_SUBS);
    s_rx.subs_pending = 2 * n;
    for (int i = 0; i < n; i++) {
        snprintf(sub, sizeof(sub), "%s/accepted", topics[i]);
        esp_mqtt_client_subscribe(c, sub, 1);
        snprintf(sub, sizeof(sub), "%s/rejected", topics[i]);
        esp_mqtt_client_subscribe(c, sub, 1);
    }
    return xEventGroupWaitBits(s_ev, BIT_SUBS, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000)) & BIT_SUBS;
}

/* Publish req to topic and wait for <topic>/accepted or /rejected. Returns the parsed body of
 * whichever came (NULL: no answer, or not JSON) and sets *rejected. */
static cJSON *request_any(esp_mqtt_client_handle_t c, const char *topic, const char *req, bool *rejected)
{
    *rejected = false;
    xEventGroupClearBits(s_ev, BIT_RESP);
    if (esp_mqtt_client_publish(c, topic, req, 0, 1, 0) < 0) {
        ESP_LOGE(TAG, "publish to %s failed", topic);
        return NULL;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_RESP | BIT_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(WAIT_MS));
    if (!(b & BIT_RESP)) {
        ESP_LOGE(TAG, "no answer on %s/accepted|rejected", topic);
        return NULL;
    }
    *rejected = strstr(s_rx.topic, "/rejected") != NULL;
    return cJSON_Parse(s_rx.buf);
}

/* --------------------------------------------------------------- shadow */

/*
 * Named shadow "config":
 *   $aws/things/<thing>/shadow/name/config/get      ""  -> accepted {state:{desired,reported}} |
 *                                                          rejected {code:404} (no shadow yet)
 *   $aws/things/<thing>/shadow/name/config/update   {state:{reported[,desired]}} -> accepted | rejected
 */
static esp_err_t shadow_sync(esp_mqtt_client_handle_t c, const char *thing, const iot_shadow_t *sh)
{
    char t_get[160], t_upd[160];
    snprintf(t_get, sizeof(t_get), "$aws/things/%s/shadow/name/config/get", thing);
    snprintf(t_upd, sizeof(t_upd), "$aws/things/%s/shadow/name/config/update", thing);
    const char *const topics[] = { t_get, t_upd };
    if (!subscribe_responses(c, topics, 2)) {
        ESP_LOGW(TAG, "shadow: subscribe failed (sensor policy?)");
        return ESP_FAIL;
    }

    bool rejected;
    cJSON *doc = request_any(c, t_get, "", &rejected);
    if (!doc) {
        return ESP_FAIL;
    }
    const cJSON *desired = NULL;
    if (rejected) {
        const cJSON *code = cJSON_GetObjectItem(doc, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 404) {
            ESP_LOGW(TAG, "shadow get rejected: %s", s_rx.buf);
            cJSON_Delete(doc);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "shadow: none yet, creating it");
    } else {
        desired = cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "state"), "desired");
        if (!cJSON_IsObject(desired)) {
            desired = NULL;
        }
    }
    if (desired && sh->apply) {
        sh->apply(desired);
    }

    cJSON *upd = cJSON_CreateObject();
    cJSON *state = cJSON_AddObjectToObject(upd, "state");
    cJSON *reported = cJSON_AddObjectToObject(state, "reported");
    sh->settings(reported);
    if (sh->status) {
        sh->status(reported);
    }
    if (!desired) {
        /* A new shadow: the current settings become "desired", so the fields are there to edit. */
        sh->settings(cJSON_AddObjectToObject(state, "desired"));
    }
    char *body = cJSON_PrintUnformatted(upd);
    cJSON_Delete(upd);
    cJSON_Delete(doc);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "shadow update: %s", body);
    cJSON *resp = request_any(c, t_upd, body, &rejected);
    cJSON_free(body);
    if (!resp || rejected) {
        ESP_LOGW(TAG, "shadow update %s", resp ? "rejected" : "not answered");
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    cJSON_Delete(resp);
    ESP_LOGI(TAG, "shadow: reported state accepted");
    return ESP_OK;
}

/* ------------------------------------------------------- fleet provisioning */

#ifdef IOT_HAVE_CLAIM
/* As request_any, for requests whose rejection is an error. Returns the accepted body. */
static cJSON *request(esp_mqtt_client_handle_t c, const char *topic, const char *req)
{
    bool rejected;
    cJSON *r = request_any(c, topic, req, &rejected);
    if (r && rejected) {
        ESP_LOGE(TAG, "%s rejected: %s", topic, s_rx.buf);
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

/*
 *   $aws/certificates/create-from-csr/json          {"certificateSigningRequest": PEM}
 *     -> accepted {certificateId, certificatePem, certificateOwnershipToken}
 *   $aws/provisioning-templates/<template>/provision/json
 *     {"certificateOwnershipToken": ..., "parameters": {"SerialNumber": <backend id>}}
 *     -> accepted {thingName}
 */
static bool fleet_provision(const char *device_id)
{
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "equi-sensor-claim-%s", device_id);
    ESP_LOGI(TAG, "fleet provisioning: template \"%s\", serial %s", FLEET_TEMPLATE, device_id);

    char *csr = NULL, *key = NULL;
    char cn[64];
    snprintf(cn, sizeof(cn), THING_PREFIX "%s", device_id);
    if (make_csr(cn, &csr, &key) != ESP_OK) {
        return false;
    }
    bool ok = false;
    cJSON *cert_resp = NULL, *prov_resp = NULL;
    char *req = NULL;
    static const char t_csr[] = "$aws/certificates/create-from-csr/json";
    static const char t_prov[] = "$aws/provisioning-templates/" FLEET_TEMPLATE "/provision/json";

    esp_mqtt_client_handle_t c = client_start(client_id, claim_crt_start, claim_key_start);
    if (!c) {
        ESP_LOGE(TAG, "claim connection failed (claim certificate active? claim policy attached?)");
        free(csr);
        free(key);
        return false;
    }
    const char *const topics[] = { t_csr, t_prov };
    if (!subscribe_responses(c, topics, 2)) {
        ESP_LOGE(TAG, "subscribe failed (claim policy?)");
        goto out;
    }

    /* 1. Certificate from our CSR */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "certificateSigningRequest", csr);
    req = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    cert_resp = req ? request(c, t_csr, req) : NULL;
    cJSON_free(req);
    req = NULL;
    const cJSON *pem = cJSON_GetObjectItem(cert_resp, "certificatePem");
    const cJSON *tok = cJSON_GetObjectItem(cert_resp, "certificateOwnershipToken");
    const cJSON *cid = cJSON_GetObjectItem(cert_resp, "certificateId");
    if (!cJSON_IsString(pem) || !cJSON_IsString(tok)) {
        goto out;
    }
    ESP_LOGI(TAG, "certificate %.12s... issued (%u bytes)", cJSON_IsString(cid) ? cid->valuestring : "?",
             (unsigned)strlen(pem->valuestring));

    /* 2. Register the thing through the template */
    r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "certificateOwnershipToken", tok->valuestring);
    cJSON *params = cJSON_AddObjectToObject(r, "parameters");
    cJSON_AddStringToObject(params, "SerialNumber", device_id);
    req = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    prov_resp = req ? request(c, t_prov, req) : NULL;
    const cJSON *thing = cJSON_GetObjectItem(prov_resp, "thingName");
    if (!cJSON_IsString(thing)) {
        goto out;
    }
    ESP_LOGI(TAG, "registered as thing \"%s\"", thing->valuestring);
    ok = identity_save(thing->valuestring, pem->valuestring, key) == ESP_OK;
out:
    client_stop(c);
    cJSON_Delete(cert_resp);
    cJSON_Delete(prov_resp);
    cJSON_free(req);
    free(csr);
    free(key);
    return ok;
}
#endif

/* --------------------------------------------------------------------- API */

esp_err_t iot_sync(const char *device_id, const iot_shadow_t *shadow)
{
    static bool warned;
#ifndef IOT_ENDPOINT
    if (!warned) {
        ESP_LOGW(TAG, "no IoT Core endpoint in this build (private/sensor-claim/endpoint.txt or "
                      "private/iot/endpoint.txt): AWS IoT off");
        warned = true;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_ev && !(s_ev = xEventGroupCreate())) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:8883", IOT_ENDPOINT);

    int free_entries = nvs_free_entries();   /* logged every round */
    (void)free_entries;
    identity_t id;
    if (!identity_load(&id)) {
#ifdef IOT_HAVE_CLAIM
        if (free_entries >= 0 && free_entries < NVS_ENTRIES_NEEDED + NVS_ENTRIES_MARGIN) {
            if (!warned) {
                ESP_LOGE(TAG, "NVS has %d free entries, the identity needs %d + %d spare: not provisioning",
                         free_entries, NVS_ENTRIES_NEEDED, NVS_ENTRIES_MARGIN);
                warned = true;
            }
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        if (!fleet_provision(device_id) || !identity_load(&id)) {
            ESP_LOGW(TAG, "fleet provisioning failed");
            return ESP_FAIL;
        }
#else
        if (!warned) {
            ESP_LOGW(TAG, "no AWS identity in NVS and no claim certificate in this build "
                          "(private/sensor-claim/): AWS IoT off");
            warned = true;
        }
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    esp_mqtt_client_handle_t c = client_start(id.thing, id.cert_pem, id.key_pem);
    if (!c) {
        ESP_LOGW(TAG, "could not connect to AWS IoT as \"%s\"", id.thing);
        identity_free(&id);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connected to AWS IoT as thing \"%s\"", id.thing);
    esp_err_t err = shadow && shadow->settings ? shadow_sync(c, id.thing, shadow) : ESP_OK;
    client_stop(c);
    identity_free(&id);
    return err;
#endif
}
