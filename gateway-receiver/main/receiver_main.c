/*
 * Equimetrics BLE gateway receiver.
 *
 * Scans continuously (100 % duty, active, no duplicate filtering) and forwards every
 * Equimetrics telemetry advert and identity scan response over the USB serial console. On request from the PC it
 * connects to one device for an ECG capture and/or to set its clock.
 *
 * Device -> PC lines:
 *   EQRX,ready
 *   EQADV,<addr>,<rssi>,<payload hex>      advert or scan response (payload after the company ID;
 *                                          E9 02.. = telemetry, E9 81.. = identity)
 *   EQHB,<uptime_s>,<adverts_forwarded>    heartbeat every 10 s
 *   EQECG,<event>,<addr>[,detail]          session events: WAIT CONN MTU READY TIMESET STARTED
 *                                          END DISC TIMEOUT BUSY ERR
 *   EQECGD,<addr>,<packet hex>             ECG notification (see ecg_service.h on the device)
 *
 * PC -> device lines:
 *   ECG,<addr>,<seconds>,<flags>,<epoch>   connect when next seen; set time if epoch != 0,
 *                                          capture if seconds != 0, then disconnect
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define COMPANY_ID       0xFFFF
#define MAGIC            0xE9
#define WAIT_ADV_S       75      /* devices advertise every 60 s */
#define ECG_PKT_END      0xEF
#define CMD_START        0x01
#define CMD_SET_TIME     0x10

/* 7a5e000X-3c2b-4a8e-9f61-6b1e4c0d5e01 */
#define EQ_UUID(x) BLE_UUID128_INIT(0x01, 0x5e, 0x0d, 0x4c, 0x1e, 0x6b, 0x61, 0x9f, \
                                    0x8e, 0x4a, 0x2b, 0x3c, (x), 0x00, 0x5e, 0x7a)
static const ble_uuid128_t k_svc_uuid  = EQ_UUID(0x01);
static const ble_uuid128_t k_ctrl_uuid = EQ_UUID(0x02);
static const ble_uuid128_t k_data_uuid = EQ_UUID(0x03);

static const char *TAG = "rx";
static uint8_t s_own_addr_type;
static volatile uint32_t s_forwarded;

typedef enum { S_IDLE, S_WAIT_ADV, S_CONNECTING, S_ACTIVE } sess_state_t;
static struct {
    volatile sess_state_t state;
    char addr[18];
    uint8_t seconds, flags;
    uint32_t epoch;
    int64_t deadline_us;
    uint16_t conn;
    uint16_t svc_start, svc_end, ctrl_h, data_h;
} s;

static void start_scan(void);
static int gap_event(struct ble_gap_event *ev, void *arg);

/* ---------------------------------------------------------------- helpers */

static void fmt_addr(const ble_addr_t *a, char out[18])
{
    const uint8_t *v = a->val;
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
}

static void print_hex_line(const char *prefix, const uint8_t *p, int n)
{
    static char hex[2 * 260 + 1];
    if (n > 260) {
        n = 260;
    }
    for (int i = 0; i < n; i++) {
        snprintf(&hex[2 * i], 3, "%02X", p[i]);
    }
    hex[2 * n] = 0;
    printf("%s,%s,%s\n", prefix, s.addr, hex);
}

static void sess_event(const char *ev, int detail)
{
    printf("EQECG,%s,%s,%d\n", ev, s.addr, detail);
}

static void sess_end(void)
{
    s.state = S_IDLE;
    s.conn = BLE_HS_CONN_HANDLE_NONE;
    start_scan();
}

static void fail(const char *why, int rc)
{
    sess_event("ERR", rc);
    ESP_LOGW(TAG, "%s rc=%d", why, rc);
    if (s.conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);   /* DISCONNECT event ends the session */
    } else {
        sess_end();
    }
}

/* Return the manufacturer-data payload after the company ID if it is ours. */
static const uint8_t *find_ours(const uint8_t *ad, int len, int *out_len)
{
    for (int i = 0; i + 1 < len;) {
        int l = ad[i];
        if (l == 0 || i + 1 + l > len) {
            break;
        }
        if (ad[i + 1] == BLE_HS_ADV_TYPE_MFG_DATA && l >= 4) {
            uint16_t cid = ad[i + 2] | (ad[i + 3] << 8);
            if (cid == COMPANY_ID && ad[i + 4] == MAGIC) {
                *out_len = l - 3;
                return &ad[i + 4];
            }
        }
        i += 1 + l;
    }
    return NULL;
}

/* --------------------------------------------------- GATT client sequence */

static int on_start_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        fail("start", err->status);
    } else {
        sess_event("STARTED", s.seconds);
    }
    return 0;
}

static void send_start_or_finish(void)
{
    if (s.seconds == 0) {
        ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);   /* time-set only */
        return;
    }
    uint8_t cmd[3] = { CMD_START, s.seconds, s.flags };
    int rc = ble_gattc_write_flat(s.conn, s.ctrl_h, cmd, sizeof(cmd), on_start_written, NULL);
    if (rc) {
        fail("write start", rc);
    }
}

static int on_time_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    sess_event("TIMESET", err->status);
    send_start_or_finish();
    return 0;
}

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        fail("subscribe", err->status);
        return 0;
    }
    sess_event("READY", 0);
    if (s.epoch) {
        uint8_t cmd[5] = { CMD_SET_TIME, s.epoch & 0xff, (s.epoch >> 8) & 0xff, (s.epoch >> 16) & 0xff, s.epoch >> 24 };
        int rc = ble_gattc_write_flat(s.conn, s.ctrl_h, cmd, sizeof(cmd), on_time_written, NULL);
        if (rc) {
            fail("write time", rc);
        }
    } else {
        send_start_or_finish();
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &k_ctrl_uuid.u) == 0) {
            s.ctrl_h = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &k_data_uuid.u) == 0) {
            s.data_h = chr->val_handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail("disc chrs", err->status);
        return 0;
    }
    if (!s.ctrl_h || !s.data_h) {
        fail("ECG characteristics missing", 0);
        return 0;
    }
    /* Subscribe: the CCCD directly follows the value handle in NimBLE's attribute layout. */
    uint8_t cccd[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(conn, s.data_h + 1, cccd, sizeof(cccd), on_subscribed, NULL);
    if (rc) {
        fail("write cccd", rc);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        s.svc_start = svc->start_handle;
        s.svc_end = svc->end_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s.svc_start) {
        fail("ECG service not found", err->status);
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn, s.svc_start, s.svc_end, on_chr, NULL);
    if (rc) {
        fail("disc chrs start", rc);
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    sess_event("MTU", mtu);
    int rc = ble_gattc_disc_svc_by_uuid(conn, &k_svc_uuid.u, on_svc, NULL);
    if (rc) {
        fail("disc svc", rc);
    }
    return 0;
}

/* --------------------------------------------------------------------- GAP */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &ev->disc;
        int n;
        const uint8_t *p = find_ours(d->data, d->length_data, &n);
        if (!p) {
            return 0;
        }
        char addr[18];
        fmt_addr(&d->addr, addr);
        static char hex[2 * 31 + 1];
        for (int i = 0; i < n; i++) {
            snprintf(&hex[2 * i], 3, "%02X", p[i]);
        }
        printf("EQADV,%s,%d,%s\n", addr, d->rssi, hex);
        s_forwarded++;

        if (s.state == S_WAIT_ADV && strcmp(addr, s.addr) == 0 &&
            (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND)) {
            s.state = S_CONNECTING;
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(s_own_addr_type, &d->addr, 3000, NULL, gap_event, NULL);
            if (rc) {
                fail("connect", rc);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s.state == S_IDLE || s.state == S_WAIT_ADV) {
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            s.conn = BLE_HS_CONN_HANDLE_NONE;
            fail("connect failed", ev->connect.status);
            return 0;
        }
        s.conn = ev->connect.conn_handle;
        s.state = S_ACTIVE;
        sess_event("CONN", 0);
        ble_gattc_exchange_mtu(s.conn, on_mtu, NULL);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[260];
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &len);
        print_hex_line("EQECGD", buf, len);
        if (len && buf[0] == ECG_PKT_END) {
            sess_event("END", 0);
            ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        sess_event("DISC", ev->disconnect.reason);
        sess_end();
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (ble_gap_disc_active()) {
        return;
    }
    struct ble_gap_disc_params p = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(100),
        .window = BLE_GAP_SCAN_ITVL_MS(100),   /* window == interval: listen 100 % of the time */
        .filter_duplicates = 0,
        .passive = 0,                          /* active: also collect the identity scan response */
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    printf("EQRX,ready\n");
    start_scan();
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------- PC commands */

static void handle_command(char *line)
{
    char *tok[6] = { 0 };
    int n = 0;
    for (char *p = strtok(line, ",\r\n"); p && n < 6; p = strtok(NULL, ",\r\n")) {
        tok[n++] = p;
    }
    if (n >= 5 && strcmp(tok[0], "ECG") == 0) {
        if (s.state != S_IDLE) {
            printf("EQECG,BUSY,%s,0\n", tok[1]);
            return;
        }
        strncpy(s.addr, tok[1], sizeof(s.addr) - 1);
        s.seconds = (uint8_t)atoi(tok[2]);
        s.flags = (uint8_t)atoi(tok[3]);
        s.epoch = (uint32_t)strtoul(tok[4], NULL, 10);
        s.svc_start = s.svc_end = s.ctrl_h = s.data_h = 0;
        s.conn = BLE_HS_CONN_HANDLE_NONE;
        s.deadline_us = esp_timer_get_time() + WAIT_ADV_S * 1000000LL;
        s.state = S_WAIT_ADV;
        sess_event("WAIT", WAIT_ADV_S);
    }
}

static void uart_task(void *arg)
{
    char line[96];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(1000)) == 1) {
            if (c == '\n' || c == '\r') {
                if (n) {
                    line[n] = 0;
                    handle_command(line);
                    n = 0;
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)c;
            }
        }
        if (s.state == S_WAIT_ADV && esp_timer_get_time() > s.deadline_us) {
            sess_event("TIMEOUT", WAIT_ADV_S);
            s.state = S_IDLE;
        }
    }
}

void app_main(void)
{
    /* NVS holds the RF calibration. This is a dev kit, so erasing on error is fine here. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Console UART also carries commands from the PC. */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    s.state = S_IDLE;
    s.conn = BLE_HS_CONN_HANDLE_NONE;
    xTaskCreate(uart_task, "uart_cmd", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_att_set_preferred_mtu(247);
    nimble_port_freertos_init(host_task);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        printf("EQHB,%lld,%lu\n", esp_timer_get_time() / 1000000, (unsigned long)s_forwarded);
    }
}
