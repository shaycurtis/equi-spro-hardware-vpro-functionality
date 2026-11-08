#include "telemetry.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
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
    put32(&out[5], t->session);
    out[9] = t->seq;
    out[10] = t->flags;
    put16(&out[11], t->battery_soc256);
    put16(&out[13], t->battery_mv);
    put16(&out[15], (uint16_t)t->temp_c100);
    out[17] = t->humidity_half;
    put32(&out[18], t->steps);
    out[22] = t->heart_rate;
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

static bool is_gateway_addr(const ble_addr_t *a)
{
    if (a->type == BLE_ADDR_PUBLIC || a->type == BLE_ADDR_PUBLIC_ID) {
        return true;
    }
    return (a->val[5] & 0xC0) == 0xC0;   /* static random; 01 = resolvable, 00 = non-resolvable */
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
         * Gateways (ESP32, Nordic, commercial units) use a fixed identity: a public or a
         * static random address (top two bits 11). Phones use rotating private addresses,
         * which are ignored so a phone nearby does not count as a gateway. */
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
    put32(&r[n], session);
    n += 4;
    for (int i = 0; i < 6; i++) {
        r[n++] = (uint8_t)(horse_id >> (8 * i));
    }
    size_t name_len = strlen(s_name);
    if (name_len && n + 2 + name_len <= sizeof(s_rsp)) {
        r[n++] = (uint8_t)(1 + name_len);
        r[n++] = BLE_HS_ADV_TYPE_COMP_NAME;
        memcpy(&r[n], s_name, name_len);
        n += name_len;
    }
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

bool telemetry_gateway_seen(uint32_t max_age_ms)
{
    int64_t t = s_gw_seen_us;
    return t >= 0 && esp_timer_get_time() - t <= (int64_t)max_age_ms * 1000;
}

uint32_t telemetry_scan_requests(void)
{
    return s_scan_reqs;
}
