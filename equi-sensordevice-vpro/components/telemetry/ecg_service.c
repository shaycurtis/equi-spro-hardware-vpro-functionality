/*
 * Equimetrics ECG GATT service (protocol v1). See ecg_service.h for the wire format.
 */
#include "ecg_service.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ecg.h"
#include "sensors.h"
#include "telemetry.h"

static const char *TAG = "ecg_svc";

/* 7a5e000X-3c2b-4a8e-9f61-6b1e4c0d5e01, little-endian byte order for NimBLE */
#define EQ_UUID(x) BLE_UUID128_INIT(0x01, 0x5e, 0x0d, 0x4c, 0x1e, 0x6b, 0x61, 0x9f, \
                                    0x8e, 0x4a, 0x2b, 0x3c, (x), 0x00, 0x5e, 0x7a)
static const ble_uuid128_t k_svc_uuid  = EQ_UUID(0x01);
static const ble_uuid128_t k_ctrl_uuid = EQ_UUID(0x02);
static const ble_uuid128_t k_data_uuid = EQ_UUID(0x03);
static const ble_uuid128_t k_info_uuid = EQ_UUID(0x04);

static uint16_t s_data_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify;

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static void notify(const uint8_t *d, size_t n)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        uint16_t conn = s_conn;
        if (conn == BLE_HS_CONN_HANDLE_NONE || !s_notify) {
            return;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
        if (!om) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int rc = ble_gatts_notify_custom(conn, s_data_handle, om);   /* consumes om */
        if (rc == 0) {
            return;
        }
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
            ESP_LOGW(TAG, "notify rc=%d", rc);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));   /* controller buffers full: back off */
    }
    ESP_LOGW(TAG, "notify dropped after retries");
}

static void on_block(const int16_t *s, size_t n, uint32_t first, void *ctx)
{
    uint8_t pkt[ECG_PKT_HDR + 2 * ECG_BLOCK_SAMPLES];
    pkt[0] = ECG_PKT_DATA;
    put32(&pkt[1], first);
    pkt[5] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        put16(&pkt[ECG_PKT_HDR + 2 * i], (uint16_t)s[i]);
    }
    notify(pkt, ECG_PKT_HDR + 2 * n);
}

static void on_done(esp_err_t status, uint32_t total, uint32_t overflows, void *ctx)
{
    uint8_t pkt[8] = { ECG_PKT_END, status == ESP_OK ? 0 : 1 };
    put32(&pkt[2], total);
    put16(&pkt[6], (uint16_t)(overflows > 0xffff ? 0xffff : overflows));
    notify(pkt, sizeof(pkt));
}

static int handle_ctrl_cmd(uint16_t conn, const uint8_t *d, uint16_t len);

/* Any command that succeeds marks the writer as a trusted gateway. */
static int handle_ctrl(uint16_t conn, const uint8_t *d, uint16_t len)
{
    int rc = handle_ctrl_cmd(conn, d, len);
    if (rc == 0) {
        telemetry_trust_peer(conn);
    }
    return rc;
}

static int handle_ctrl_cmd(uint16_t conn, const uint8_t *d, uint16_t len)
{
    switch (len ? d[0] : 0) {
    case ECG_CMD_START: {
        if (len < 3) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (!s_notify) {
            return ECG_ATT_ERR_NOT_SUBSCRIBED;
        }
        if (ble_att_mtu(conn) < ECG_PKT_HDR + 2 * ECG_BLOCK_SAMPLES + 3) {
            return ECG_ATT_ERR_MTU_TOO_SMALL;
        }
        uint8_t seconds = d[1];
        bool test = d[2] & ECG_FLAG_TEST_SIGNAL;
        ecg_request_t req = { .seconds = seconds, .test_signal = test, .on_block = on_block, .on_done = on_done };

        uint8_t start[10] = { ECG_PKT_START };
        put16(&start[1], ECG_RATE_HZ);
        float uv = (float)ECG_UV_PER_LSB;
        memcpy(&start[3], &uv, 4);
        start[7] = seconds;
        start[8] = d[2];
        start[9] = ECG_BLOCK_SAMPLES;

        esp_err_t err = ecg_start(&req);
        if (err == ESP_ERR_INVALID_STATE) {
            return ECG_ATT_ERR_BUSY;
        }
        if (err != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        notify(start, sizeof(start));
        ESP_LOGI(TAG, "ECG capture requested: %u s%s", seconds, test ? " (test signal)" : "");
        return 0;
    }
    case ECG_CMD_STOP:
        ecg_stop();
        return 0;
    case ECG_CMD_PAIR:
        return 0;   /* "I am your gateway": only the trust side effect */
    case ECG_CMD_SET_TIME: {
        if (len < 5) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint32_t epoch = d[1] | (d[2] << 8) | (d[3] << 16) | ((uint32_t)d[4] << 24);
        return sensors_set_time(epoch) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    default:
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(uuid, &k_ctrl_uuid.u) == 0) {
        uint8_t buf[16];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        return handle_ctrl(conn, buf, len);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(uuid, &k_info_uuid.u) == 0) {
        uint8_t info[9] = { ECG_PROTOCOL_VERSION };
        put16(&info[1], ECG_RATE_HZ);
        float uv = (float)ECG_UV_PER_LSB;
        memcpy(&info[3], &uv, 4);
        info[7] = ECG_BLOCK_SAMPLES;
        info[8] = ECG_MAX_SECONDS;
        return os_mbuf_append(ctxt->om, info, sizeof(info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &k_ctrl_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &k_data_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_data_handle },
            { .uuid = &k_info_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

int ecg_service_register(void)
{
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(k_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(k_svcs);
    }
    return rc;
}

void ecg_service_on_connect(uint16_t conn)
{
    (void)conn;   /* the gateway is whoever subscribes to Data (a phone may be connected too) */
}

void ecg_service_on_disconnect(uint16_t conn)
{
    if (conn == s_conn) {
        s_notify = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        if (ecg_busy()) {
            ESP_LOGW(TAG, "gateway disconnected during capture, stopping");
            ecg_stop();
        }
    }
}

void ecg_service_on_subscribe(uint16_t conn, uint16_t attr, bool notify_on)
{
    if (attr != s_data_handle) {
        return;
    }
    if (notify_on) {
        s_conn = conn;
        s_notify = true;
    } else if (conn == s_conn) {
        s_notify = false;
    }
}
