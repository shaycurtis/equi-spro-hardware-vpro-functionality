/*
 * BLE: NimBLE active scan (as gateway-receiver/main/receiver_main.c), only our devices.
 *
 * Ours = manufacturer data with company ID 0xFFFF and magic 0xE9 (equi-sensordevice-vpro
 * components/telemetry/include/telemetry.h). Byte 1 after the company ID is the kind: 0x02..
 * telemetry advert, 0x80 bit set = identity (the encrypted scan response). Bytes 2..4 are the
 * device's 6-digit ID in clear in both.
 *
 * Bursts are grouped as gateway/mqtt_publisher.py does: every copy of one telemetry advert from one
 * device (same bytes) is one burst; GW_BURST_SETTLE_MS after its first copy it is queued with the
 * device's latest identity scan response. Never connects, never sends anything but scan requests.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "gw.h"

#define COMPANY_ID     0xFFFF
#define MAGIC          0xE9
#define KIND_IDENTITY  0x80
#define MAX_ACTIVE     16      /* bursts being collected at once */
#define MAX_DEVICES    64      /* devices remembered (identity, last heard) */

static const char *TAG = "ble";

typedef struct {
    bool used;
    uint8_t dev[3];
    uint8_t addr[6];
    int64_t last_seen_us;
    uint8_t rsp_len;
    uint8_t rsp[GW_AD_MAX];
    int8_t rsp_rssi;
    int64_t rsp_rx_us;
} device_t;

static struct {
    bool used;
    uint8_t dev[3];
    gw_burst_t b;
} s_active[MAX_ACTIVE];

static device_t s_devs[MAX_DEVICES];
static SemaphoreHandle_t s_lock;
static uint8_t s_own_addr_type;
static uint32_t s_bursts;

static void start_scan(void);

/* Manufacturer-data AD structure if it is ours: returns its start, *len = whole structure. */
static const uint8_t *find_ours(const uint8_t *ad, int len, int *out_len)
{
    for (int i = 0; i + 1 < len;) {
        int l = ad[i];
        if (l == 0 || i + 1 + l > len) {
            break;
        }
        if (ad[i + 1] == BLE_HS_ADV_TYPE_MFG_DATA && l >= 7) {   /* type, company, magic, kind, id */
            uint16_t cid = ad[i + 2] | (ad[i + 3] << 8);
            if (cid == COMPANY_ID && ad[i + 4] == MAGIC) {
                *out_len = 1 + l;
                return &ad[i];
            }
        }
        i += 1 + l;
    }
    return NULL;
}

static device_t *device_for(const uint8_t dev[3], int64_t now)
{
    device_t *free_slot = NULL, *oldest = &s_devs[0];
    for (int i = 0; i < MAX_DEVICES; i++) {
        device_t *d = &s_devs[i];
        if (d->used && memcmp(d->dev, dev, 3) == 0) {
            return d;
        }
        if (!d->used && !free_slot) {
            free_slot = d;
        }
        if (d->last_seen_us < oldest->last_seen_us) {
            oldest = d;
        }
    }
    device_t *d = free_slot ? free_slot : oldest;
    memset(d, 0, sizeof(*d));
    d->used = true;
    memcpy(d->dev, dev, 3);
    d->last_seen_us = now;
    return d;
}

static void finish(int i)
{
    device_t *d = NULL;
    for (int k = 0; k < MAX_DEVICES; k++) {
        if (s_devs[k].used && memcmp(s_devs[k].dev, s_active[i].dev, 3) == 0) {
            d = &s_devs[k];
        }
    }
    gw_burst_t *b = &s_active[i].b;
    if (d && d->rsp_len) {   /* the latest identity, as the PC gateway */
        b->rsp_len = d->rsp_len;
        memcpy(b->rsp, d->rsp, d->rsp_len);
        b->rsp_rssi = d->rsp_rssi;
        b->rsp_rx_us = d->rsp_rx_us;
    }
    gw_queue_push(b);
    s_bursts++;
    s_active[i].used = false;
    int best = -127;
    for (int k = 0; k < b->ncopies; k++) {
        best = b->copies[k].rssi > best ? b->copies[k].rssi : best;
    }
    ESP_LOGI(TAG, "burst %02X%02X%02X: %d copies, best %d dBm%s", s_active[i].dev[0], s_active[i].dev[1],
             s_active[i].dev[2], b->ncopies, best, b->rsp_len ? ", with identity" : ", no identity yet");
    gw_led_pulse();
}

static void on_ours(const struct ble_gap_disc_desc *d, const uint8_t *ad, int n)
{
    int64_t now = esp_timer_get_time();
    const uint8_t *dev = &ad[6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    device_t *dv = device_for(dev, now);
    dv->last_seen_us = now;
    memcpy(dv->addr, d->addr.val, 6);

    if (ad[5] & KIND_IDENTITY) {
        dv->rsp_len = n;
        memcpy(dv->rsp, ad, n);
        dv->rsp_rssi = d->rssi;
        dv->rsp_rx_us = now;
        xSemaphoreGive(s_lock);
        return;
    }

    int slot = -1, free_slot = -1, oldest = -1;
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (s_active[i].used) {
            gw_burst_t *b = &s_active[i].b;
            if (b->adv_len == n && memcmp(b->adv, ad, n) == 0 && memcmp(b->addr, d->addr.val, 6) == 0) {
                slot = i;
                break;
            }
            if (oldest < 0 || b->first_rx_us < s_active[oldest].b.first_rx_us) {
                oldest = i;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (slot < 0) {
        if (free_slot < 0) {
            finish(oldest);   /* table full: queue the oldest early */
            free_slot = oldest;
        }
        slot = free_slot;
        s_active[slot].used = true;
        memcpy(s_active[slot].dev, dev, 3);
        gw_burst_t *b = &s_active[slot].b;
        memset(b, 0, sizeof(*b));
        memcpy(b->addr, d->addr.val, 6);
        b->adv_len = n;
        memcpy(b->adv, ad, n);
        b->first_rx_us = now;
    }
    gw_burst_t *b = &s_active[slot].b;
    if (b->ncopies < GW_MAX_COPIES) {
        b->copies[b->ncopies].rssi = d->rssi;
        b->copies[b->ncopies].dt_ms = (uint32_t)((now - b->first_rx_us) / 1000);
        b->ncopies++;
    }
    xSemaphoreGive(s_lock);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        int n;
        const uint8_t *ad = find_ours(ev->disc.data, ev->disc.length_data, &n);
        if (ad) {
            on_ours(&ev->disc, ad, n);
        }
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
    if (ble_gap_disc_active()) {
        return;
    }
    struct ble_gap_disc_params p = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(CONFIG_GW_SCAN_ITVL_MS),
        .window = BLE_GAP_SCAN_WIN_MS(CONFIG_GW_SCAN_WINDOW_MS),
        .filter_duplicates = 0,   /* every copy: RSSI per copy, as the PC gateway */
        .passive = 0,             /* active: the session + HorseID are only in the scan response */
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "active scan started (%d/%d ms)", CONFIG_GW_SCAN_WINDOW_MS, CONFIG_GW_SCAN_ITVL_MS);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void flush_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_ACTIVE; i++) {
            if (s_active[i].used && now - s_active[i].b.first_rx_us >= CONFIG_GW_BURST_SETTLE_MS * 1000LL) {
                finish(i);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

void gw_ble_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    xTaskCreate(flush_task, "burst_flush", 3072, NULL, 5, NULL);
}

int gw_ble_devices_heard(int within_s)
{
    int64_t since = esp_timer_get_time() - within_s * 1000000LL;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devs[i].used && s_devs[i].last_seen_us >= since) {
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t gw_ble_bursts(void)
{
    return s_bursts;
}
