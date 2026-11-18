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
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
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
static esp_err_t shadow_sync(esp_mqtt_client_handle_t c, const char *thing, const iot_cfg_t *sh)
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

/* ------------------------------------------------- firmware updates (Jobs) */

#define OTA_KEY          "ota"     /* NVS (namespace "iot"): the job in progress */
#define OTA_ID_LEN       65        /* job IDs are up to 64 characters */
#define OTA_VER_LEN      32        /* esp_app_desc_t.version */
#define OTA_HTTP_TIMEOUT 30000

typedef struct {
    char job_id[OTA_ID_LEN];
    char version[OTA_VER_LEN];
    uint8_t tries;               /* downloads started for this job */
} ota_state_t;

static volatile bool s_restart;

static const char *running_version(void)
{
    return esp_app_get_description()->version;
}

static bool ota_state_load(ota_state_t *st)
{
    memset(st, 0, sizeof(*st));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*st);
    bool ok = nvs_get_blob(h, OTA_KEY, st, &len) == ESP_OK && len == sizeof(*st);
    nvs_close(h);
    st->job_id[OTA_ID_LEN - 1] = st->version[OTA_VER_LEN - 1] = 0;
    return ok && st->job_id[0];
}

static void ota_state_save(const ota_state_t *st)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, OTA_KEY, st, sizeof(*st)) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

static void ota_state_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, OTA_KEY) == ESP_OK) {   /* only our own key */
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

/* A new image runs "pending verify" until confirmed; reset before that and the bootloader goes
 * back to the previous one. Getting to AWS is the confirmation. */
static void confirm_new_image(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t got = esp_ota_get_state_partition(running, &state);
    /* "pending verify" right after an update proves the bootloader has rollback; "new" means it
     * does not (the update still runs, but a broken image would not roll back by itself).
     * The factory slot has no state (ESP_ERR_NOT_SUPPORTED). */
    static const char *const names[] = { "new", "pending verify", "valid", "invalid", "aborted" };
    const char *name = "none";
    if (got == ESP_OK) {
        switch (state) {
        case ESP_OTA_IMG_NEW:            name = names[0]; break;
        case ESP_OTA_IMG_PENDING_VERIFY: name = names[1]; break;
        case ESP_OTA_IMG_VALID:          name = names[2]; break;
        case ESP_OTA_IMG_INVALID:        name = names[3]; break;
        case ESP_OTA_IMG_ABORTED:        name = names[4]; break;
        default:                         name = "undefined"; break;
        }
    }
    ESP_LOGI(TAG, "running from %s, OTA state %s", running->label, name);
    if (got == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "new firmware %s confirmed (reached AWS IoT): %s", running_version(), esp_err_to_name(err));
    }
}

/* $aws/things/<thing>/jobs/<id>/update with a status and one detail (statusDetails are strings). */
static void job_update(esp_mqtt_client_handle_t c, const char *thing, const char *job_id, const char *status,
                       const char *key, const char *value)
{
    char topic[200];
    snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/%s/update", thing, job_id);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", status);
    cJSON *d = cJSON_AddObjectToObject(r, "statusDetails");
    cJSON_AddStringToObject(d, "fw", running_version());
    if (key && value) {
        cJSON_AddStringToObject(d, key, value);
    }
    char *body = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!body) {
        return;
    }
    bool rejected;
    cJSON *resp = request_any(c, topic, body, &rejected);
    ESP_LOGI(TAG, "job %s -> %s%s", job_id, status, !resp ? " (no answer)" : rejected ? " (rejected)" : "");
    if (resp && rejected) {
        ESP_LOGW(TAG, "job update rejected: %s", s_rx.buf);
    }
    cJSON_Delete(resp);
    cJSON_free(body);
}

/*
 * Check the jobs. On return *url (heap) is set if an image should be downloaded now, with
 * *version the version it must carry. The job is left IN_PROGRESS: SUCCEEDED is reported from the
 * new firmware after the restart.
 */
static esp_err_t jobs_sync(esp_mqtt_client_handle_t c, const char *thing, char **url, char version[OTA_VER_LEN])
{
    *url = NULL;
    char t_start[160], t_upd[160];
    snprintf(t_start, sizeof(t_start), "$aws/things/%s/jobs/start-next", thing);
    snprintf(t_upd, sizeof(t_upd), "$aws/things/%s/jobs/+/update", thing);
    const char *const topics[] = { t_start, t_upd };
    if (!subscribe_responses(c, topics, 2)) {
        ESP_LOGW(TAG, "jobs: subscribe failed (sensor policy?)");
        return ESP_FAIL;
    }
    const char *running = running_version();

    /* An update started before the restart and now running: done. */
    ota_state_t st;
    bool have = ota_state_load(&st);
    if (have && strcmp(st.version, running) == 0) {
        job_update(c, thing, st.job_id, "SUCCEEDED", "result", "updated");
        ota_state_clear();
        have = false;
    }

    /* The next job (QUEUED or already IN_PROGRESS); start-next marks it IN_PROGRESS. */
    char req[96];
    snprintf(req, sizeof(req), "{\"statusDetails\":{\"fw\":\"%s\"}}", running);
    bool rejected;
    cJSON *resp = request_any(c, t_start, req, &rejected);
    if (!resp || rejected) {
        ESP_LOGW(TAG, "jobs: start-next %s", resp ? "rejected" : "not answered");
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    const cJSON *ex = cJSON_GetObjectItem(resp, "execution");
    const cJSON *jid = cJSON_GetObjectItem(ex, "jobId");
    if (!cJSON_IsString(jid)) {
        ESP_LOGI(TAG, "jobs: none pending");
        if (have) {
            ota_state_clear();   /* that job was cancelled or finished elsewhere */
        }
        cJSON_Delete(resp);
        return ESP_OK;
    }
    const char *job_id = jid->valuestring;
    const cJSON *doc = cJSON_GetObjectItem(ex, "jobDocument");
    const cJSON *op = cJSON_GetObjectItem(doc, "operation");
    const cJSON *ver = cJSON_GetObjectItem(doc, "version");
    const cJSON *u = cJSON_GetObjectItem(doc, "url");
    ESP_LOGI(TAG, "job %s: operation %s, version %s (running %s)", job_id,
             cJSON_IsString(op) ? op->valuestring : "?", cJSON_IsString(ver) ? ver->valuestring : "?", running);

    if (!cJSON_IsString(op) || strcmp(op->valuestring, "ota") != 0) {
        job_update(c, thing, job_id, "REJECTED", "reason", "unknown operation");
    } else if (!cJSON_IsString(ver) || !ver->valuestring[0] || strlen(ver->valuestring) >= OTA_VER_LEN ||
               !cJSON_IsString(u) || strncmp(u->valuestring, "https://", 8) != 0) {
        job_update(c, thing, job_id, "REJECTED", "reason", "needs \"version\" and an https \"url\"");
    } else if (strcmp(ver->valuestring, running) == 0) {
        job_update(c, thing, job_id, "SUCCEEDED", "result", "already running");
        ota_state_clear();
    } else {
        uint8_t tries = have && strcmp(st.job_id, job_id) == 0 ? st.tries : 0;
        if (tries >= IOT_OTA_MAX_TRIES) {
            job_update(c, thing, job_id, "FAILED", "reason", "download or new firmware failed 3 times");
            ota_state_clear();
        } else {
            ota_state_t next = { .tries = (uint8_t)(tries + 1) };
            strlcpy(next.job_id, job_id, sizeof(next.job_id));
            strlcpy(next.version, ver->valuestring, sizeof(next.version));
            ota_state_save(&next);   /* before the download: a failure or rollback counts */
            char n[8];
            snprintf(n, sizeof(n), "%u", next.tries);
            job_update(c, thing, job_id, "IN_PROGRESS", "try", n);
            strlcpy(version, ver->valuestring, OTA_VER_LEN);
            *url = strdup(u->valuestring);
        }
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

/* Download the image into the next OTA slot and set it to boot. Its version must be `version`;
 * esp_https_ota_finish -> esp_ota_end checks the secure boot signature. */
static esp_err_t ota_download(const char *url, const char *version)
{
    esp_http_client_config_t hc = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT,
        .buffer_size = 4096,
        .buffer_size_tx = 8192,       /* the pre-signed URL goes in the request line */
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t oc = { .http_config = &hc };
    esp_https_ota_handle_t h = NULL;
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "firmware %s: downloading", version);
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware download: %s", esp_err_to_name(err));
        return err;
    }
    esp_app_desc_t d;
    err = esp_https_ota_get_img_desc(h, &d);
    if (err == ESP_OK && strncmp(d.version, version, sizeof(d.version)) != 0) {
        ESP_LOGE(TAG, "firmware download: the image is version %s, the job says %s", d.version, version);
        err = ESP_ERR_INVALID_VERSION;
    }
    int next_pct = 25;
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        err = ESP_OK;
        int total = esp_https_ota_get_image_size(h), got = esp_https_ota_get_image_len_read(h);
        if (total > 0 && got * 100 / total >= next_pct) {
            ESP_LOGI(TAG, "firmware download: %d %%", got * 100 / total);
            next_pct += 25;
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(h);
        ESP_LOGE(TAG, "firmware download failed: %s (the job retries next round)", esp_err_to_name(err));
        return err;
    }
    err = esp_https_ota_finish(h);   /* verifies the image (signature) and sets it to boot */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware image rejected: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "firmware %s written in %lld ms: restart to run it", version,
             (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

/* --------------------------------------------------------------------- API */

esp_err_t iot_sync(const char *device_id, const iot_cfg_t *cfg)
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
    ESP_LOGI(TAG, "connected to AWS IoT as thing \"%s\" (firmware %s)", id.thing, running_version());
    confirm_new_image();   /* reaching AWS is this firmware's health check */
    esp_err_t err = cfg && cfg->settings ? shadow_sync(c, id.thing, cfg) : ESP_OK;
    char *ota_url = NULL;
    char ota_version[OTA_VER_LEN] = "";
    esp_err_t jerr = jobs_sync(c, id.thing, &ota_url, ota_version);
    if (err == ESP_OK) {
        err = jerr;
    }
    client_stop(c);
    identity_free(&id);
    if (ota_url) {
        /* After the MQTT connection is closed: one TLS session at a time. */
        if (cfg && cfg->updating) {
            cfg->updating();
        }
        if (ota_download(ota_url, ota_version) == ESP_OK) {
            s_restart = true;
        }
        free(ota_url);
    }
    return err;
#endif
}

bool iot_restart_required(void)
{
    return s_restart;
}
