#include "telemetry.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "ecg_service.h"
#include "live_service.h"

void ble_store_config_init(void);   /* NimBLE bond store in NVS (no public header) */

static const char *TAG = "telemetry";

static uint8_t s_own_addr_type;
static char s_name[24];
static uint8_t s_rsp[31];
static size_t s_rsp_len;
static volatile bool s_synced;
static bool s_adv_configured;
static volatile int64_t s_gw_seen_us = -1;   /* last scan request from a gateway */
static volatile uint32_t s_scan_reqs;

/* Added to the chip's BT MAC (VPro adds 1, to get past iOS caching). Bump it to make phones see
 * a new device after a name change (iOS keeps the old name per address): now 4 (EQUI_15B0B4
 * again, after the spoofed-ID test on 3). */
#define BLE_ADDR_OFFSET 5
#define ADV_INSTANCE 0       /* telemetry bursts (gateway): MSD advert + encrypted identity */
#define APP_ADV_INSTANCE 1   /* the app: connectable, with the name the app filters on */
#define APP_ADV_ITVL_MS 1000 /* continuous, so the app finds us any time (as VPro) */

static bool s_app_adv_configured;
static void app_adv_start(void);

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
    out[20] = (uint8_t)((t->position & 0x07) | ((t->positions_seen >> 1) << 3));
    put16(&out[21], (uint16_t)t->horse_temp_c100);
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
    app_adv_start();
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
            ESP_LOGI(TAG, "connected (handle %d)", ev->connect.conn_handle);
            ecg_service_on_connect(ev->connect.conn_handle);
            /* As the VPro firmware: ask for a 30-60 ms interval (supervision timeout 4 s
             * rather than VPro's 180 ms, which is very tight). */
            struct ble_gap_upd_params up = { .itvl_min = 24, .itvl_max = 48, .latency = 0,
                                             .supervision_timeout = 400 };
            ble_gap_update_params(ev->connect.conn_handle, &up);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (handle %d, reason 0x%x)", ev->disconnect.conn.conn_handle,
                 ev->disconnect.reason);
        ecg_service_on_disconnect(ev->disconnect.conn.conn_handle);
        live_service_on_disconnect(ev->disconnect.conn.conn_handle);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ecg_service_on_subscribe(ev->subscribe.conn_handle, ev->subscribe.attr_handle, ev->subscribe.cur_notify);
        live_service_on_subscribe(ev->subscribe.conn_handle, ev->subscribe.attr_handle, ev->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* We display (DisplayOnly IO capability), the phone's user types the passkey. */
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = TELEMETRY_BLE_PASSKEY };
            int rc = ble_sm_inject_io(ev->passkey.conn_handle, &io);
            ESP_LOGI(TAG, "pairing (handle %d): passkey entry%s", ev->passkey.conn_handle,
                     rc ? " failed" : "");
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            ESP_LOGI(TAG, "link %d: %s%s%s", ev->enc_change.conn_handle,
                     ev->enc_change.status == 0 ? "encrypted" : "encryption failed",
                     d.sec_state.authenticated ? ", authenticated" : "", d.sec_state.bonded ? ", bonded" : "");
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone lost its bond (e.g. "forget device") but we still have ours: drop ours
         * and pair again. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
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
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* The app advert stops when someone connects through it: keep it going. (The
         * telemetry instance completes at the end of every burst, as intended.) */
        if (ev->adv_complete.instance == APP_ADV_INSTANCE) {
            app_adv_start();
        }
        break;
    default:
        break;
    }
    return 0;
}

/*
 * NimBLE keeps one local IRK in NVS, filed under the public (identity) address. After the
 * address changes (a BLE_ADDR_OFFSET bump, or the SPro firmware's own address) the old entry
 * fills the only slot, so the new one cannot be saved: "ble_store_config_write_local_irk rc=27"
 * (BLE_HS_ESTORE_CAP) at every boot and a fresh IRK each time. Drop entries for other addresses
 * before the host starts; bonds (peer keys) are left alone.
 */
#define STALE_IRK_MAX 4

typedef struct {
    ble_addr_t ours;
    ble_addr_t stale[STALE_IRK_MAX];
    int n;
} irk_scan_t;

static int irk_scan(int obj_type, union ble_store_value *v, void *arg)
{
    irk_scan_t *s = arg;
    if (memcmp(v->local_irk.addr.val, s->ours.val, sizeof(s->ours.val)) != 0 && s->n < STALE_IRK_MAX) {
        s->stale[s->n++] = v->local_irk.addr;
    }
    return 0;
}

static void drop_stale_local_irks(const uint8_t mac[6])
{
    irk_scan_t s = { .ours.type = BLE_ADDR_PUBLIC };
    for (int i = 0; i < 6; i++) {
        s.ours.val[i] = mac[5 - i];   /* NimBLE addresses are little-endian */
    }
    ble_store_iterate(BLE_STORE_OBJ_TYPE_LOCAL_IRK, irk_scan, &s);
    for (int i = 0; i < s.n; i++) {
        struct ble_store_key_local_irk key = { .addr = s.stale[i] };
        const uint8_t *a = s.stale[i].val;
        int rc = ble_store_delete_local_irk(&key);
        ESP_LOGI(TAG, "removed the local IRK of old address %02X:%02X:%02X:%02X:%02X:%02X (rc=%d)",
                 a[5], a[4], a[3], a[2], a[1], a[0], rc);
    }
}

bool telemetry_ble_set_address(uint8_t mac_out[6])
{
    /* As the VPro firmware: Bluetooth address = the chip's BT MAC + 1. Its comment: a
     * workaround for iOS caching (a new address makes iOS see a new device). esp_read_mac()
     * returns the eFuse-derived address, so this is the same on every boot. */
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        return false;
    }
    mac[5] += BLE_ADDR_OFFSET;
    esp_iface_mac_addr_set(mac, ESP_MAC_BT);
    if (mac_out) {
        memcpy(mac_out, mac, 6);
    }
    return true;
}

esp_err_t telemetry_ble_init(const char *name)
{
    uint8_t mac[6] = { 0 };
    bool have_mac = telemetry_ble_set_address(mac);
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
    rc = live_service_register();
    if (rc != 0) {
        ESP_LOGE(TAG, "live services register rc=%d", rc);
        return ESP_FAIL;
    }
    /* Pairing for the app's live services: passkey (we display, the phone enters it), LE
     * Secure Connections, bonded (keys in NVS). The gateway's services need no pairing. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    if (have_mac) {
        drop_stale_local_irks(mac);
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

esp_err_t telemetry_ble_set_identity(uint32_t device_id, uint64_t session, uint64_t horse_id)
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
    uint8_t pt[14];
    for (int i = 0; i < 8; i++) {
        pt[i] = (uint8_t)(session >> (8 * i));
    }
    for (int i = 0; i < 6; i++) {
        pt[8 + i] = (uint8_t)(horse_id >> (8 * i));
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
    if (s_synced && ble_gap_ext_adv_active(APP_ADV_INSTANCE)) {
        ble_gap_ext_adv_stop(APP_ADV_INSTANCE);
    }
}

/* Second advertising instance for the Equimetrics app (legacy PDUs, so every phone sees it),
 * laid out as the VPro firmware's NimBLE-cpp advert: advert = flags + Heart Rate (16-bit,
 * complete) + Raw ECG service (128-bit, incomplete list: the other services did not fit),
 * scan response = complete name ("EQUI_...", the app's filter). Same address as the
 * telemetry bursts. */
static void app_adv_start(void)
{
    if (!s_synced || ble_gap_ext_adv_active(APP_ADV_INSTANCE)) {
        return;
    }
    int rc;
    if (!s_app_adv_configured) {
        struct ble_gap_ext_adv_params p = {
            .connectable = 1,
            .scannable = 1,
            .legacy_pdu = 1,
            .itvl_min = BLE_GAP_ADV_ITVL_MS(APP_ADV_ITVL_MS),
            .itvl_max = BLE_GAP_ADV_ITVL_MS(APP_ADV_ITVL_MS),
            .own_addr_type = s_own_addr_type,
            .primary_phy = BLE_HCI_LE_PHY_1M,
            .secondary_phy = BLE_HCI_LE_PHY_1M,
            .tx_power = 127,
        };
        rc = ble_gap_ext_adv_configure(APP_ADV_INSTANCE, &p, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "app adv configure rc=%d", rc);
            return;
        }
        static const uint8_t adv[25] = {
            2, BLE_HS_ADV_TYPE_FLAGS, BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
            3, BLE_HS_ADV_TYPE_COMP_UUIDS16, 0x0D, 0x18,               /* Heart Rate 0x180D */
            17, BLE_HS_ADV_TYPE_INCOMP_UUIDS128,                       /* 18a6e117-...-ff2015f7d6ef */
            0xef, 0xd6, 0xf7, 0x15, 0x20, 0xff, 0x5c, 0x97, 0xc2, 0x4b, 0xf1, 0x39, 0x17, 0xe1, 0xa6, 0x18,
        };
        size_t n = sizeof(adv);
        uint8_t rsp[31];
        size_t rn = 0, len = strlen(s_name);
        if (len > sizeof(rsp) - 2) {
            len = sizeof(rsp) - 2;
        }
        rsp[rn++] = 1 + len;
        rsp[rn++] = BLE_HS_ADV_TYPE_COMP_NAME;
        memcpy(&rsp[rn], s_name, len);
        rn += len;
        struct os_mbuf *m = os_msys_get_pkthdr(n, 0);
        struct os_mbuf *r = os_msys_get_pkthdr(rn, 0);
        if (!m || !r || os_mbuf_append(m, adv, n) || os_mbuf_append(r, rsp, rn)) {
            if (m) {
                os_mbuf_free_chain(m);
            }
            if (r) {
                os_mbuf_free_chain(r);
            }
            return;
        }
        rc = ble_gap_ext_adv_set_data(APP_ADV_INSTANCE, m);
        if (rc == 0) {
            rc = ble_gap_ext_adv_rsp_set_data(APP_ADV_INSTANCE, r);
        } else {
            os_mbuf_free_chain(r);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "app adv data rc=%d", rc);
            return;
        }
        s_app_adv_configured = true;
        ESP_LOGI(TAG, "app advert \"%s\" every %d ms", s_name, APP_ADV_ITVL_MS);
    }
    rc = ble_gap_ext_adv_start(APP_ADV_INSTANCE, 0, 0);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "app adv start rc=%d", rc);
    }
}

void telemetry_device_setup_code(uint32_t device_id, char out[9])
{
    /* SHA-256(fleet key | "EQ-POP" | device id, big-endian 3 bytes), first 4 bytes as hex. */
    uint8_t msg[16 + 6 + 3];
    memcpy(msg, fleet_key_start, 16);
    memcpy(&msg[16], "EQ-POP", 6);
    msg[22] = (device_id >> 16) & 0xff;
    msg[23] = (device_id >> 8) & 0xff;
    msg[24] = device_id & 0xff;
    uint8_t h[32];
    size_t hl = 0;
    psa_crypto_init();
    if (psa_hash_compute(PSA_ALG_SHA_256, msg, sizeof(msg), h, sizeof(h), &hl) != PSA_SUCCESS) {
        memset(h, 0, sizeof(h));
    }
    snprintf(out, 9, "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
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
