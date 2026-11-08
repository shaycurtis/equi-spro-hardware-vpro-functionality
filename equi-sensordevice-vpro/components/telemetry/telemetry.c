#include "telemetry.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "ecg_service.h"

static const char *TAG = "telemetry";

static uint8_t s_own_addr_type;
static char s_name[24];
static uint8_t s_rsp[31];
static size_t s_rsp_len;
static volatile bool s_synced;
static bool s_adv_configured;
static volatile int64_t s_gw_seen_us = -1;   /* last scan request from a gateway */
static volatile uint32_t s_scan_reqs;

#define ADV_INSTANCE 0

/*
 * Trusted gateways, learned: a central that connects and writes a command (time set, ECG,
 * pair) is remembered here and in NVS (namespace "vpro", our own; the SPro "system" data is
 * never touched). Only scan requests from these addresses count as "a gateway hears us".
 */
#define MAX_GATEWAYS  4
#define NVS_NS        "vpro"
#define NVS_KEY_GW    "gateways"
static ble_addr_t s_gw[MAX_GATEWAYS];
static int s_ngw;

/* ------------------------------------------------------------------ encode */

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

void telemetry_encode(const telemetry_t *t, uint8_t out[TELEMETRY_PAYLOAD_LEN])
{
    out[0] = TELEMETRY_MAGIC;
    out[1] = TELEMETRY_VERSION;
    out[2] = (t->device_id >> 16) & 0xff;
    out[3] = (t->device_id >> 8) & 0xff;
    out[4] = t->device_id & 0xff;
    out[5] = (uint8_t)t->session;      /* session_tag */
    out[6] = t->seq;
    out[7] = t->flags;
    put16(&out[8], t->battery_soc256);
    put16(&out[10], t->battery_mv);
    put16(&out[12], (uint16_t)t->temp_c100);
    out[14] = t->humidity_half;
    put32(&out[15], t->steps);
    out[19] = t->heart_rate;
}

/* ------------------------------------------------------------ fleet key */

/* 16-byte fleet key, embedded at build time from <repo>/private/fleet_key.bin (git-ignored,
 * never leaves the build PC; flash encryption protects it on the device). */
extern const uint8_t fleet_key_start[] asm("_binary_fleet_key_bin_start");
extern const uint8_t fleet_key_end[] asm("_binary_fleet_key_bin_end");

#define CCM_TAG_LEN   4
#define CCM_ALG       PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN)

static psa_key_id_t s_fleet_key;

static esp_err_t fleet_key_init(void)
{
    if (fleet_key_end - fleet_key_start != 16) {
        ESP_LOGE(TAG, "fleet key must be 16 bytes (got %d)", (int)(fleet_key_end - fleet_key_start));
        return ESP_ERR_INVALID_SIZE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a, CCM_ALG);
    psa_status_t st = psa_import_key(&a, fleet_key_start, 16, &s_fleet_key);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "fleet key import failed (%d)", (int)st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------- BLE */

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    ESP_LOGI(TAG, "BLE ready, address %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    s_synced = true;
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool same_addr(const ble_addr_t *a, const ble_addr_t *b)
{
    /* Identity types 2/3 (public/random ID) match their plain types 0/1. */
    return (a->type & 1) == (b->type & 1) && memcmp(a->val, b->val, 6) == 0;
}

static bool is_gateway_addr(const ble_addr_t *a)
{
    for (int i = 0; i < s_ngw; i++) {
        if (same_addr(&s_gw[i], a)) {
            return true;
        }
    }
    return false;
}

static void gateways_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* nothing learned yet */
    }
    size_t len = sizeof(s_gw);
    if (nvs_get_blob(h, NVS_KEY_GW, s_gw, &len) == ESP_OK) {
        s_ngw = (int)(len / sizeof(s_gw[0]));
    }
    nvs_close(h);
    for (int i = 0; i < s_ngw; i++) {
        const uint8_t *v = s_gw[i].val;
        ESP_LOGI(TAG, "trusted gateway %02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
    }
}

void telemetry_trust_peer(uint16_t conn_handle)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn_handle, &d) != 0) {
        return;
    }
    ble_addr_t a = d.peer_id_addr;
    a.type &= 1;
    if (s_ngw && same_addr(&s_gw[0], &a)) {
        return;   /* already the most recent: nothing to save */
    }
    /* Most recent first; drop the oldest when full. */
    int i = 0;
    while (i < s_ngw && !same_addr(&s_gw[i], &a)) {
        i++;
    }
    bool is_new = i == s_ngw;
    if (is_new && s_ngw < MAX_GATEWAYS) {
        s_ngw++;
    }
    for (int k = (i < MAX_GATEWAYS ? i : MAX_GATEWAYS - 1); k > 0; k--) {
        s_gw[k] = s_gw[k - 1];
    }
    s_gw[0] = a;   /* its next scan request counts as "heard" (and delivers the identity) */
    const uint8_t *v = a.val;
    ESP_LOGI(TAG, "%s gateway %02X:%02X:%02X:%02X:%02X:%02X", is_new ? "learned" : "re-confirmed",
             v[5], v[4], v[3], v[2], v[1], v[0]);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY_GW, s_gw, s_ngw * sizeof(s_gw[0])) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

bool telemetry_has_trusted_gateway(void)
{
    return s_ngw > 0;
}

/* Distinct scanners seen in the current burst (diagnostics). */
#define MAX_SCANNERS 6
static struct { ble_addr_t a; uint16_t n; } s_scanners[MAX_SCANNERS];
static int s_nscanners;
static uint16_t s_scanners_dropped;

static void note_scanner(const ble_addr_t *a)
{
    for (int i = 0; i < s_nscanners; i++) {
        if (ble_addr_cmp(&s_scanners[i].a, a) == 0) {
            s_scanners[i].n++;
            return;
        }
    }
    if (s_nscanners < MAX_SCANNERS) {
        s_scanners[s_nscanners].a = *a;
        s_scanners[s_nscanners].n = 1;
        s_nscanners++;
    } else {
        s_scanners_dropped++;
    }
}

void telemetry_log_scanners(void)
{
    if (!s_nscanners) {
        ESP_LOGI(TAG, "no scan requests in this burst");
    }
    for (int i = 0; i < s_nscanners; i++) {
        const uint8_t *v = s_scanners[i].a.val;
        ESP_LOGI(TAG, "scanner %02X:%02X:%02X:%02X:%02X:%02X type %d x%u -> %s", v[5], v[4], v[3], v[2],
                 v[1], v[0], s_scanners[i].a.type, s_scanners[i].n,
                 is_gateway_addr(&s_scanners[i].a) ? "trusted gateway" : "not trusted");
    }
    if (s_scanners_dropped) {
        ESP_LOGI(TAG, "+%u scan requests from other scanners", s_scanners_dropped);
    }
    s_nscanners = 0;
    s_scanners_dropped = 0;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            ESP_LOGI(TAG, "gateway connected (handle %d)", ev->connect.conn_handle);
            ecg_service_on_connect(ev->connect.conn_handle);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "gateway disconnected (reason 0x%x)", ev->disconnect.reason);
        ecg_service_on_disconnect(ev->disconnect.conn.conn_handle);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ecg_service_on_subscribe(ev->subscribe.conn_handle, ev->subscribe.attr_handle, ev->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        break;
    case BLE_GAP_EVENT_SCAN_REQ_RCVD:
        /* An actively scanning gateway sends a scan request for each advert it hears.
         * Only learned (trusted) gateways count: other scanners nearby (smart-home hubs,
         * phones) also send scan requests. */
        note_scanner(&ev->scan_req_rcvd.scan_addr);
        if (is_gateway_addr(&ev->scan_req_rcvd.scan_addr)) {
            s_gw_seen_us = esp_timer_get_time();
            s_scan_reqs++;
        }
        break;
    default:
        break;   /* ADV_COMPLETE at the end of each burst is expected */
    }
    return 0;
}

esp_err_t telemetry_ble_init(const char *name)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(name);
    strncpy(s_name, name, sizeof(s_name) - 1);
    int rc = ecg_service_register();
    if (rc != 0) {
        ESP_LOGE(TAG, "ECG service register rc=%d", rc);
        return ESP_FAIL;
    }
    ble_att_set_preferred_mtu(247);
    gateways_load();
    err = fleet_key_init();
    if (err != ESP_OK) {
        return err;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static int set_rsp(void)
{
    struct os_mbuf *m = os_msys_get_pkthdr(s_rsp_len, 0);
    if (!m || os_mbuf_append(m, s_rsp, s_rsp_len) != 0) {
        if (m) {
            os_mbuf_free_chain(m);
        }
        return BLE_HS_ENOMEM;
    }
    int rc = ble_gap_ext_adv_rsp_set_data(ADV_INSTANCE, m);
    if (rc != 0) {
        ESP_LOGW(TAG, "ext_adv_rsp_set_data rc=%d", rc);
    }
    return rc;
}

esp_err_t telemetry_ble_burst(const uint8_t payload[TELEMETRY_PAYLOAD_LEN], int duration_ms)
{
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t adv[31];
    size_t n = 0;
    adv[n++] = 2;    adv[n++] = BLE_HS_ADV_TYPE_FLAGS;
    adv[n++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv[n++] = 1 + 2 + TELEMETRY_PAYLOAD_LEN;
    adv[n++] = BLE_HS_ADV_TYPE_MFG_DATA;
    adv[n++] = TELEMETRY_COMPANY_ID & 0xff;
    adv[n++] = TELEMETRY_COMPANY_ID >> 8;
    memcpy(&adv[n], payload, TELEMETRY_PAYLOAD_LEN);
    n += TELEMETRY_PAYLOAD_LEN;

    if (ble_gap_ext_adv_active(ADV_INSTANCE)) {
        ble_gap_ext_adv_stop(ADV_INSTANCE);
    }
    int rc;
    if (!s_adv_configured) {
        /* Extended-advertising API with legacy PDUs (ADV_IND): on air this is identical to
         * plain legacy advertising, but the controller reports scan requests. */
        struct ble_gap_ext_adv_params p = {
            .connectable = 1,                  /* the gateway connects for ECG / time */
            .scannable = 1,
            .legacy_pdu = 1,
            .scan_req_notif = 1,
            .itvl_min = BLE_GAP_ADV_ITVL_MS(100),
            .itvl_max = BLE_GAP_ADV_ITVL_MS(100),
            .own_addr_type = s_own_addr_type,
            .primary_phy = BLE_HCI_LE_PHY_1M,
            .secondary_phy = BLE_HCI_LE_PHY_1M,
            .tx_power = 127,                   /* no preference: controller default (+15 dBm) */
        };
        int8_t txp = 0;
        rc = ble_gap_ext_adv_configure(ADV_INSTANCE, &p, &txp, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ext_adv_configure rc=%d", rc);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "advertising configured (legacy PDUs, scan-request notify, %d dBm)", txp);
        s_adv_configured = true;
    }
    struct os_mbuf *m = os_msys_get_pkthdr(n, 0);
    if (!m || os_mbuf_append(m, adv, n) != 0) {
        if (m) {
            os_mbuf_free_chain(m);
        }
        return ESP_ERR_NO_MEM;
    }
    rc = ble_gap_ext_adv_set_data(ADV_INSTANCE, m);   /* takes ownership of m */
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_set_data rc=%d", rc);
        return ESP_FAIL;
    }
    if (s_rsp_len) {
        set_rsp();
    }
    rc = ble_gap_ext_adv_start(ADV_INSTANCE, duration_ms / 10, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_start rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t telemetry_ble_set_identity(uint32_t device_id, uint32_t session, uint64_t horse_id)
{
    uint8_t *r = s_rsp;
    size_t n = 0;
    r[n++] = 1 + 2 + TELEMETRY_IDENTITY_LEN;
    r[n++] = BLE_HS_ADV_TYPE_MFG_DATA;
    r[n++] = TELEMETRY_COMPANY_ID & 0xff;
    r[n++] = TELEMETRY_COMPANY_ID >> 8;
    r[n++] = TELEMETRY_MAGIC;
    r[n++] = TELEMETRY_IDENTITY;
    r[n++] = (device_id >> 16) & 0xff;
    r[n++] = (device_id >> 8) & 0xff;
    r[n++] = device_id & 0xff;
    uint32_t rnd = esp_random();      /* new nonce for every identity change */
    put32(&r[n], rnd);
    const uint8_t *aad = &r[4];       /* magic .. nonce (9 bytes) */
    n += 4;

    uint8_t nonce[7] = { r[6], r[7], r[8], r[9], r[10], r[11], r[12] };   /* device_id + rnd */
    uint8_t pt[10];
    put32(&pt[0], session);
    for (int i = 0; i < 6; i++) {
        pt[4 + i] = (uint8_t)(horse_id >> (8 * i));
    }
    size_t out_len = 0;
    psa_status_t st = psa_aead_encrypt(s_fleet_key, CCM_ALG, nonce, sizeof(nonce), aad, 9, pt, sizeof(pt),
                                       &r[n], sizeof(s_rsp) - n, &out_len);
    if (st != PSA_SUCCESS || out_len != sizeof(pt) + CCM_TAG_LEN) {
        ESP_LOGE(TAG, "identity encrypt failed (%d)", (int)st);
        s_rsp_len = 0;
        return ESP_FAIL;
    }
    n += out_len;
    s_rsp_len = n;
    if (!s_synced || !s_adv_configured) {
        return ESP_OK;   /* applied on the next burst */
    }
    return set_rsp() == 0 ? ESP_OK : ESP_FAIL;
}

void telemetry_ble_stop(void)
{
    if (s_synced && ble_gap_ext_adv_active(ADV_INSTANCE)) {
        ble_gap_ext_adv_stop(ADV_INSTANCE);
    }
}

int64_t telemetry_gateway_last_seen_us(void)
{
    return s_gw_seen_us;
}

bool telemetry_gateway_seen(uint32_t max_age_ms)
{
    int64_t t = s_gw_seen_us;
    return t >= 0 && esp_timer_get_time() - t <= (int64_t)max_age_ms * 1000;
}

uint32_t telemetry_scan_requests(void)
{
    return s_scan_reqs;
}
