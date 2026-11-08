/*
 * Equimetrics BLE gateway receiver.
 *
 * Scans continuously (100 % duty, passive, no duplicate filtering) and forwards every
 * Equimetrics telemetry advert over the USB serial console, one line per advert:
 *
 *   EQADV,<addr>,<rssi>,<payload hex>      payload = manufacturer data after the company ID
 *   EQHB,<uptime_s>,<adverts_forwarded>    heartbeat every 10 s
 *
 * The PC gateway (gateway/gateway.py --serial COMx) parses these lines.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define COMPANY_ID   0xFFFF
#define MAGIC        0xE9

static const char *TAG = "rx";
static uint8_t s_own_addr_type;
static volatile uint32_t s_forwarded;

static void start_scan(void);

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
        char hex[2 * 31 + 1];
        for (int i = 0; i < n; i++) {
            snprintf(&hex[2 * i], 3, "%02X", p[i]);
        }
        const uint8_t *a = d->addr.val;
        printf("EQADV,%02X:%02X:%02X:%02X:%02X:%02X,%d,%s\n",
               a[5], a[4], a[3], a[2], a[1], a[0], d->rssi, hex);
        s_forwarded++;
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(100),
        .window = BLE_GAP_SCAN_ITVL_MS(100),   /* window == interval: listen 100 % of the time */
        .filter_duplicates = 0,
        .passive = 1,
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

void app_main(void)
{
    /* NVS holds the RF calibration. This is a dev kit, so erasing on error is fine here. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        printf("EQHB,%lld,%lu\n", esp_timer_get_time() / 1000000, (unsigned long)s_forwarded);
    }
}
