/*
 * VPro-compatible live ECG, heart rate and ECG control services. See live_service.h.
 */
#include "live_service.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "ecg.h"
#include "telemetry.h"

static const char *TAG = "live";

/* UUIDs as in the VPro platform's BLE_Local.h (bytes little-endian for NimBLE). */
static const ble_uuid128_t k_rawecg_svc = BLE_UUID128_INIT(0xef, 0xd6, 0xf7, 0x15, 0x20, 0xff, 0x5c, 0x97, 0xc2, 0x4b, 0xf1, 0x39, 0x17, 0xe1, 0xa6, 0x18);   /* 18a6e117-39f1-4bc2-975c-ff2015f7d6ef */
static const ble_uuid128_t k_rawecg_data = BLE_UUID128_INIT(0x4a, 0xa3, 0x1e, 0xeb, 0x51, 0x27, 0xbc, 0x82, 0x21, 0x44, 0x00, 0x41, 0xc5, 0x2f, 0xdc, 0xc2);  /* c2dc2fc5-4100-4421-82bc-2751eb1ea34a */
static const ble_uuid128_t k_resp_svc = BLE_UUID128_INIT(0x92, 0x76, 0x14, 0x80, 0x84, 0xb1, 0x3d, 0x8c, 0x2e, 0x44, 0x5f, 0x18, 0x35, 0x12, 0x74, 0x46);     /* 46741235-185f-442e-8c3d-b18480147692 */
static const ble_uuid128_t k_resp_sig = BLE_UUID128_INIT(0xbe, 0x32, 0xf5, 0xe8, 0x3c, 0x19, 0xf7, 0x93, 0xf7, 0x48, 0x6b, 0xf4, 0x3d, 0xf8, 0x0a, 0x95);     /* 950af83d-f46b-48f7-93f7-193ce8f532be */
static const ble_uuid128_t k_ctl_svc = BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x56, 0xbe, 0xee, 0x11, 0x89, 0x50, 0x6e, 0xcc, 0xf7, 0x0d);      /* 0df7cc6e-5089-11ee-be56-0242ac120002 */
static const ble_uuid128_t k_ctl_cmd = BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x56, 0xbe, 0xee, 0x11, 0x89, 0x50, 0x16, 0xcf, 0xf7, 0x0d);      /* 0df7cf16-5089-11ee-be56-0242ac120002 */
static const ble_uuid128_t k_ctl_dur = BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x56, 0xbe, 0xee, 0x11, 0x89, 0x50, 0x94, 0xd3, 0xf7, 0x0d);      /* 0df7d394-5089-11ee-be56-0242ac120002 */
static const ble_uuid128_t k_ctl_recid = BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x56, 0xbe, 0xee, 0x11, 0x89, 0x50, 0xf2, 0xd4, 0xf7, 0x0d);    /* 0df7d4f2-5089-11ee-be56-0242ac120002 */
static const ble_uuid128_t k_ctl_status = BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x62, 0xb9, 0xee, 0x11, 0x02, 0x78, 0xbe, 0x36, 0xc1, 0xf2);   /* f2c136be-7802-11ee-b962-0242ac120002 */
static const ble_uuid128_t k_ctl_totdur = BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x62, 0xb9, 0xee, 0x11, 0x02, 0x78, 0x84, 0x39, 0xc1, 0xf2);   /* f2c13984-7802-11ee-b962-0242ac120002 */
#define UUID_HR_SVC   0x180D
#define UUID_HR_MEAS  0x2A37
#define UUID_FORMAT   0x2904

/* As the VPro firmware: everything needs an encrypted (bonded) link, subscriptions included.
 * We ask for MITM when pairing, so phones with a keyboard use the passkey. */
#define F_R  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)
#define F_W  (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
#define F_N  (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC)

#define SAMPLE_BYTES   16
#define MAX_PER_PKT    10        /* as the VPro firmware */
#define TICK_US        1000000   /* heart rate, leads, recording timer */

static uint16_t h_ecg, h_resp, h_hr, h_recid, h_status, h_totdur;

/* The app's connection (one at a time) and what it subscribed to. */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_sub_ecg, s_sub_hr, s_sub_recid, s_sub_status, s_sub_totdur;
static bool s_live_on;

/* Live ECG packet being filled (ECG task). */
static uint8_t s_pkt[MAX_PER_PKT * SAMPLE_BYTES];
static size_t s_npkt, s_per_pkt = MAX_PER_PKT;
static volatile uint8_t s_status_bits;            /* leads bits for every sample */
static volatile uint32_t s_sent, s_dropped;

/* App-started recording (ECG control service). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_rec_id;
static uint32_t s_total_s;
static char s_status[24] = "none";
static bool s_running;
static bool s_file;                                /* a file is being written for it */
static int64_t s_until_us;                         /* 0 = no time limit */
static telemetry_rec_start_fn s_rec_start;
static telemetry_rec_stop_fn s_rec_stop;

void telemetry_set_recording_hooks(telemetry_rec_start_fn start, telemetry_rec_stop_fn stop)
{
    s_rec_start = start;
    s_rec_stop = stop;
}

/* End the recording: save or delete its file; the next one gets a new ID. */
static void end_recording(bool keep)
{
    bool file = s_file;
    s_file = false;
    if (file && s_rec_stop) {
        s_rec_stop(keep);
    }
    s_rec_id = 0;
}

static esp_timer_handle_t s_tick;
static uint32_t s_ticks;
static uint8_t s_bpm;

static void apply(void);

/* ------------------------------------------------------------------ notify */

static bool link_secure(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    return conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted;
}

/* Non-blocking: if the controller's buffers are full the packet is dropped (never stall the
 * ECG task, which also feeds the SD recording). */
static bool notify(uint16_t handle, const void *data, size_t len)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;
    }
    return ble_gatts_notify_custom(conn, handle, om) == 0;   /* consumes om */
}

static void put_be(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    }
}

static void notify_recid(void)
{
    if (s_sub_recid) {
        uint8_t b[8];
        put_be(b, s_rec_id, 8);
        notify(h_recid, b, sizeof(b));
    }
}

static void notify_totdur(void)
{
    if (s_sub_totdur) {
        uint8_t b[4];
        put_be(b, s_total_s, 4);
        notify(h_totdur, b, sizeof(b));
    }
}

static void set_status(const char *st)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_status, st, sizeof(s_status));
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "recording status: %s", st);
    if (s_sub_status) {
        notify(h_status, st, strlen(st));
    }
}

/* ----------------------------------------------------------------- live ECG */

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ECG task: pack 16-byte samples (the platform's ECGData_t) and notify every s_per_pkt. */
static void on_live(const int32_t *raw, size_t n, uint16_t rr, void *ctx)
{
    int64_t t_last = now_ms();   /* the newest sample of the batch */
    for (size_t i = 0; i < n; i++) {
        if (s_npkt == 0) {
            uint16_t mtu = s_conn != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn) : 23;
            size_t fit = mtu > 3 ? (mtu - 3) / SAMPLE_BYTES : 1;
            s_per_pkt = fit < 1 ? 1 : fit > MAX_PER_PKT ? MAX_PER_PKT : fit;
        }
        uint8_t *p = &s_pkt[s_npkt * SAMPLE_BYTES];
        int64_t ts = t_last - (int64_t)(n - 1 - i) * 1000 / ECG_LIVE_RATE_HZ;
        uint16_t r = (i == n - 1) ? rr : 0;
        memcpy(p, &ts, 8);
        memcpy(p + 8, &raw[i], 4);
        p[12] = s_status_bits;
        p[13] = 0;
        memcpy(p + 14, &r, 2);
        if (++s_npkt >= s_per_pkt) {
            if (notify(h_ecg, s_pkt, s_npkt * SAMPLE_BYTES)) {
                s_sent++;
            } else {
                s_dropped++;
            }
            s_npkt = 0;
        }
    }
}

/* -------------------------------------------------------------- 1 s tick */

static void tick(void *arg)
{
    uint8_t bpm, beats;
    ecg_hr_get(5, &bpm, &beats);
    s_status_bits = bpm ? (3u << 2) : 0;   /* leads "both, valid" while beats are detected */
    ecg_hr_get(10, &s_bpm, &beats);
    if (s_sub_hr) {
        uint8_t m[2] = { 0, s_bpm };       /* flags: uint8 bpm, no contact / energy / RR */
        notify(h_hr, m, sizeof(m));
    }

    bool finished = false;
    portENTER_CRITICAL(&s_mux);
    if (s_running && s_until_us && esp_timer_get_time() >= s_until_us) {
        s_running = false;
        s_total_s = 0;
        finished = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (finished) {
        end_recording(true);
        set_status("finished");
        notify_totdur();
    }

    if (s_live_on && ++s_ticks % 60 == 0) {
        ESP_LOGI(TAG, "live ECG: %lu notifications sent, %lu dropped in the last minute (MTU %u)",
                 (unsigned long)s_sent, (unsigned long)s_dropped,
                 s_conn != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn) : 0);
        s_sent = s_dropped = 0;
    }
    if (!s_live_on && !s_sub_hr && !s_running) {
        esp_timer_stop(s_tick);   /* nothing needs it: no extra wake-ups */
    }
}

/* Start / stop the live tap and the tick to match the subscriptions (host task). */
static void apply(void)
{
    bool want = s_sub_ecg && link_secure(s_conn);
    if (want && !s_live_on) {
        s_npkt = 0;
        s_sent = s_dropped = 0;
        ecg_live_start(on_live, NULL);
        s_live_on = true;
        ESP_LOGI(TAG, "live ECG on (%u sps, MTU %u)", ECG_LIVE_RATE_HZ, ble_att_mtu(s_conn));
    } else if (!want && s_live_on) {
        ecg_live_stop();
        s_live_on = false;
        ESP_LOGI(TAG, "live ECG off");
    }
    if ((s_live_on || s_sub_hr || s_running) && !esp_timer_is_active(s_tick)) {
        esp_timer_start_periodic(s_tick, TICK_US);
    }
}

/* ------------------------------------------------------------ ECG control */

static uint64_t be_value(const uint8_t *d, uint16_t len, int max)
{
    uint64_t v = 0;
    for (int i = 0; i < len && i < max; i++) {
        v = (v << 8) | d[i];
    }
    return v;
}

static void command(const char *cmd)
{
    ESP_LOGI(TAG, "command \"%s\"", cmd);
    if (!strcmp(cmd, "start")) {
        if (s_running) {
            set_status("running");   /* already recording */
            return;
        }
        if (!s_rec_id) {
            s_rec_id = (uint64_t)now_ms();   /* the app did not set one: use the start time */
            notify_recid();
        }
        esp_err_t err = s_rec_start ? s_rec_start(s_rec_id, s_total_s) : ESP_ERR_NOT_SUPPORTED;
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "recording not started: %s", esp_err_to_name(err));
            set_status(err == ESP_ERR_NOT_FOUND ? "error: no SD card" : "error: cannot record");
            return;
        }
        portENTER_CRITICAL(&s_mux);
        s_file = err == ESP_OK;
        s_running = true;
        s_until_us = s_total_s ? esp_timer_get_time() + (int64_t)s_total_s * 1000000 : 0;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "recording %llu for %lu s (0 = until stopped)%s", (unsigned long long)s_rec_id,
                 (unsigned long)s_total_s, s_file ? "" : ": continuous session file, live view only");
        set_status("running");
        apply();
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "discard")) {
        bool keep = cmd[0] == 's';
        portENTER_CRITICAL(&s_mux);
        bool was = s_running;
        s_running = false;
        s_total_s = 0;
        portEXIT_CRITICAL(&s_mux);
        if (was) {
            end_recording(keep);
        }
        set_status(keep ? "stopped" : "discarded");
        notify_totdur();
    } else {
        set_status("error: invalid command");
    }
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC ? ctxt->dsc->uuid : ctxt->chr->uuid;
    s_conn = conn;   /* the client using these services is "the app" */

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        /* HR measurement presentation format: uint8, exponent 0, unit 0x27AF (bpm), namespace 1 */
        static const uint8_t fmt[7] = { 0x04, 0x00, 0xAF, 0x27, 0x01, 0x00, 0x00 };
        return os_mbuf_append(ctxt->om, fmt, sizeof(fmt)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t b[24];
        size_t n = 0;
        if (ble_uuid_cmp(uuid, &k_ctl_status.u) == 0) {
            portENTER_CRITICAL(&s_mux);
            n = strlcpy((char *)b, s_status, sizeof(b));
            portEXIT_CRITICAL(&s_mux);
        } else if (ble_uuid_cmp(uuid, &k_ctl_totdur.u) == 0) {
            put_be(b, s_total_s, 4);
            n = 4;
        } else if (ble_uuid_cmp(uuid, &k_ctl_recid.u) == 0) {
            put_be(b, s_rec_id, 8);
            n = 8;
        } else if (ble_uuid_cmp(uuid, BLE_UUID16_DECLARE(UUID_HR_MEAS)) == 0) {
            b[0] = 0;
            b[1] = s_bpm;
            n = 2;
        }   /* ECG data / respiration: notify-only in practice, read is empty */
        return os_mbuf_append(ctxt->om, b, n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t d[17];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, d, sizeof(d) - 1, &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (ble_uuid_cmp(uuid, &k_ctl_cmd.u) == 0) {
            while (len && (d[len - 1] == 0 || d[len - 1] == '\n' || d[len - 1] == '\r')) {
                len--;   /* tolerate a trailing NUL / newline */
            }
            d[len] = 0;
            command((const char *)d);
        } else if (ble_uuid_cmp(uuid, &k_ctl_dur.u) == 0) {
            uint32_t add = (uint32_t)be_value(d, len, 4);
            portENTER_CRITICAL(&s_mux);
            s_total_s += add;   /* cumulative, as the VPro firmware */
            if (s_running && s_total_s) {
                s_until_us = esp_timer_get_time() + (int64_t)s_total_s * 1000000;
            }
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "duration +%lu s = %lu s", (unsigned long)add, (unsigned long)s_total_s);
            notify_totdur();
        } else if (ble_uuid_cmp(uuid, &k_ctl_recid.u) == 0) {
            if (s_running) {
                ESP_LOGW(TAG, "recording ID not changed: recording in progress");
            } else {
                s_rec_id = be_value(d, len, 8);
                ESP_LOGI(TAG, "recording ID %llu", (unsigned long long)s_rec_id);
            }
            notify_recid();
        } else {
            return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_HR_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(UUID_HR_MEAS), .access_cb = access_cb, .flags = F_R | F_N,
              .val_handle = &h_hr,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(UUID_FORMAT), .access_cb = access_cb,
                    .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC },
                  { 0 },
              } },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_rawecg_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &k_rawecg_data.u, .access_cb = access_cb, .flags = F_R | F_N, .val_handle = &h_ecg },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_resp_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &k_resp_sig.u, .access_cb = access_cb, .flags = F_R | F_N, .val_handle = &h_resp },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_ctl_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &k_ctl_cmd.u, .access_cb = access_cb, .flags = F_W },
            { .uuid = &k_ctl_dur.u, .access_cb = access_cb, .flags = F_W },
            { .uuid = &k_ctl_recid.u, .access_cb = access_cb, .flags = F_R | F_W | F_N, .val_handle = &h_recid },
            { .uuid = &k_ctl_status.u, .access_cb = access_cb, .flags = F_R | F_N, .val_handle = &h_status },
            { .uuid = &k_ctl_totdur.u, .access_cb = access_cb, .flags = F_R | F_N, .val_handle = &h_totdur },
            { 0 },
        },
    },
    { 0 },
};

int live_service_register(void)
{
    const esp_timer_create_args_t a = { .callback = tick, .name = "live" };
    if (esp_timer_create(&a, &s_tick) != ESP_OK) {
        return -1;
    }
    int rc = ble_gatts_count_cfg(k_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(k_svcs);
    }
    return rc;
}

void live_service_on_subscribe(uint16_t conn, uint16_t attr, bool on)
{
    volatile bool *f = attr == h_ecg ? &s_sub_ecg : attr == h_hr ? &s_sub_hr : attr == h_recid ? &s_sub_recid :
                       attr == h_status ? &s_sub_status : attr == h_totdur ? &s_sub_totdur : NULL;
    if (!f) {
        return;   /* not ours (e.g. the gateway's ECG service) */
    }
    if (on) {
        s_conn = conn;
    } else if (conn != s_conn) {
        return;
    }
    *f = on;
    apply();
}

void live_service_on_disconnect(uint16_t conn)
{
    if (conn != s_conn) {
        return;
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_sub_ecg = s_sub_hr = s_sub_recid = s_sub_status = s_sub_totdur = false;
    apply();
    /* A recording keeps its timer (it finishes on time); the app sees the status on reconnect. */
}
