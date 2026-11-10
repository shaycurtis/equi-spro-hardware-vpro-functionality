/*
 * Charging-time backend calls. See cloud.h.
 */
#include "cloud.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "provisioning.h"
#include "storage.h"
#include "api_env.h"   /* <repo>/private: API_DATA_URL, API_DATA_KEY (not in git) */

static const char *TAG = "cloud";

#define CONNECT_TIMEOUT_MS 30000   /* per try: association + DHCP (one router took ~16 s for an IP) */
#define CONNECT_TRIES      2       /* per network per round (a first "failed" is often transient) */
#define RETRY_PENDING_S    60      /* connected, but recordings are still on the card */
#define SNTP_TIMEOUT_MS    15000
#define HTTP_TIMEOUT_MS    30000   /* as VPro */
#define UPLOAD_ATTEMPTS    3       /* per file and connection, as VPro */
#define UPLOAD_RETRY_MS    3000
#define UPLOAD_CHUNK       4096
#define RESP_MAX           4096    /* prepare returns a long pre-signed URL */
#define BIT_GOT_IP         BIT0
#define BIT_FAILED         BIT1

static cloud_cfg_t s_cfg;
static EventGroupHandle_t s_ev;
static char s_ssid[33];

/* ------------------------------------------------------------------ Wi-Fi */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_ev, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static esp_err_t wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* the saved networks live in our own NVS list */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

#define SCAN_MAX_APS 20   /* scan records kept (heap); the rest are ignored */

typedef struct {
    int index;    /* in the saved list (prov_network_get) */
    int rssi;     /* best seen, or INT_MIN when not seen */
    int tries;
} try_t;

/* One blocking all-channel scan, then the try order: saved networks seen in the scan, strongest
 * first, CONNECT_TRIES each; then the ones not seen (maybe a hidden SSID), one try each. If the
 * scan fails: every network newest first, CONNECT_TRIES each (as before). Returns the count. */
static int plan_tries(try_t order[PROV_MAX_NETWORKS])
{
    int n = prov_network_count();
    if (n > PROV_MAX_NETWORKS) {
        n = PROV_MAX_NETWORKS;
    }
    for (int i = 0; i < n; i++) {
        order[i] = (try_t){ .index = i, .rssi = INT_MIN, .tries = CONNECT_TRIES };
    }
    wifi_scan_config_t sc = { 0 };   /* all channels, active scan, hidden SSIDs not listed */
    uint16_t found = 0;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_num(&found);
    }
    uint16_t nrec = found < SCAN_MAX_APS ? found : SCAN_MAX_APS;
    wifi_ap_record_t *recs = NULL;
    if (err == ESP_OK && nrec > 0) {
        recs = calloc(nrec, sizeof(*recs));
        err = recs ? esp_wifi_scan_get_ap_records(&nrec, recs) : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();   /* frees the driver's list (no-op if empty) */
        free(recs);
        ESP_LOGW(TAG, "scan failed (%s): trying the saved networks newest first", esp_err_to_name(err));
        return n;
    }
    if (!recs) {
        esp_wifi_clear_ap_list();   /* nothing found: free the driver's list */
    }
    for (int i = 0; i < n; i++) {
        char ssid[33], pass[65];
        if (!prov_network_get(order[i].index, ssid, pass)) {
            continue;
        }
        for (int r = 0; r < nrec; r++) {
            if (strcmp((const char *)recs[r].ssid, ssid) == 0 && recs[r].rssi > order[i].rssi) {
                order[i].rssi = recs[r].rssi;
            }
        }
        order[i].tries = order[i].rssi == INT_MIN ? 1 : CONNECT_TRIES;
    }
    free(recs);
    /* Seen first, strongest first; not seen after, newest first (stable insertion sort). */
    for (int i = 1; i < n; i++) {
        try_t t = order[i];
        int j = i - 1;
        while (j >= 0 && order[j].rssi < t.rssi) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = t;
    }
    ESP_LOGI(TAG, "scan: %u access point(s); try order:", found);
    for (int i = 0; i < n; i++) {
        char ssid[33], pass[65];
        if (!prov_network_get(order[i].index, ssid, pass)) {
            continue;
        }
        if (order[i].rssi == INT_MIN) {
            ESP_LOGI(TAG, "  %d. \"%s\" not seen (hidden?), %d try", i + 1, ssid, order[i].tries);
        } else {
            ESP_LOGI(TAG, "  %d. \"%s\" %d dBm, %d tries", i + 1, ssid, order[i].rssi, order[i].tries);
        }
    }
    return n;
}

/* Scan, then try the saved networks in range, strongest first (see plan_tries). */
static bool wifi_connect(void)
{
    if (esp_wifi_start() != ESP_OK) {
        return false;
    }
    try_t order[PROV_MAX_NETWORKS];
    int n = plan_tries(order);
    for (int o = 0; o < n; o++) {
        char ssid[33], pass[65];
        if (!prov_network_get(order[o].index, ssid, pass)) {
            continue;
        }
        for (int t = 0; t < order[o].tries; t++) {
            wifi_config_t wc = { 0 };
            strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
            strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
            wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
            wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;        /* as VPro: the strongest AP */
            wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;    /* when several share the SSID */
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
            ESP_LOGI(TAG, "connecting to \"%s\" (try %d of %d)...", ssid, t + 1, order[o].tries);
            esp_wifi_connect();
            EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdTRUE, pdFALSE,
                                                pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
            if (b & BIT_GOT_IP) {
                strlcpy(s_ssid, ssid, sizeof(s_ssid));
                ESP_LOGI(TAG, "connected to \"%s\"", ssid);
                return true;
            }
            ESP_LOGW(TAG, "\"%s\": %s", ssid, (b & BIT_FAILED) ? "failed" : "timed out");
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    esp_wifi_stop();
    return false;
}

static void wifi_off(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
}

/* ------------------------------------------------------------------- time */

static bool time_sync(void)
{
    static bool started;
    if (!started) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&c);
        started = true;
    } else {
        esp_netif_sntp_start();
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP: no answer; using the RTC time");
        return false;
    }
    time_t now = time(NULL);
    ESP_LOGI(TAG, "time synced: %lld", (long long)now);
    if (s_cfg.set_time) {
        s_cfg.set_time((uint32_t)now);
    }
    return true;
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------- HTTPS */

/* POST to API_DATA_URL + path with the data key. body NULL: empty body and no Content-Type (as
 * VPro's process request). resp (optional) receives the response text. Returns the HTTP status
 * (< 0: no reply). */
static int api_post(const char *path, const char *body, char *resp, size_t resp_len)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/%s", API_DATA_URL, path);
    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* verify the server certificate */
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return -1;
    }
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    esp_http_client_set_header(c, "x-api-key", API_DATA_KEY);
    int status = -1;
    size_t len = body ? strlen(body) : 0;
    esp_err_t err = esp_http_client_open(c, len);
    if (err == ESP_OK && (len == 0 || esp_http_client_write(c, body, len) == (int)len)) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char small[257];
        char *buf = resp ? resp : small;
        size_t cap = resp ? resp_len : sizeof(small);
        int n = esp_http_client_read_response(c, buf, cap - 1);
        buf[n > 0 ? n : 0] = 0;
        ESP_LOGI(TAG, "POST /%s -> %d %.200s", path, status, buf);
    } else {
        ESP_LOGW(TAG, "POST /%s failed: %s", path, esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    return status;
}

static int post_json(const char *path, const char *body)
{
    return api_post(path, body, NULL, 0);
}

/* ------------------------------------------------------- charging telemetry */

static int send_charging(const cloud_status_t *st)
{
    int64_t t = now_ms();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "deviceId", s_cfg.device_id);
    cJSON_AddNumberToObject(j, "sessionId", (double)t);   /* each report its own ID: send time */
    cJSON_AddNumberToObject(j, "timeStamp", (double)t);
    cJSON *d = cJSON_AddObjectToObject(j, "device");
    cJSON_AddStringToObject(d, "hwVersion", s_cfg.hw_version);
    cJSON_AddStringToObject(d, "fwVersion", s_cfg.fw_version);
    cJSON_AddStringToObject(d, "lastUpdate", s_cfg.last_update);
    cJSON_AddNumberToObject(d, "batteryLevel", st->battery_pct);
    char tc[16];
    snprintf(tc, sizeof(tc), "%.2f", st->temperature_c);
    cJSON_AddRawToObject(d, "temperature", tc);   /* 2 decimals, as VPro */
    cJSON_AddNumberToObject(d, "humidity", (int)(st->humidity_pct + 0.5f));
    cJSON_AddStringToObject(d, "chargerStatus", st->charging ? "Charging" : "ChargingComplete");
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "ssid", s_ssid);
    cJSON_AddArrayToObject(w, "fileProgress");
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return -1;
    }
    ESP_LOGI(TAG, "charging telemetry: %s", body);
    int status = post_json("api/sessions/events/charging", body);
    cJSON_free(body);
    return status;
}

/* ------------------------------------------------------------ file uploads */

/*
 * Recordings on the SD card, as the VPro firmware's FileUploadMgr / FileUploader:
 *   /sdcard/sessions/<sessionId>/<prefix>_<deviceId>_<chip|Unknown>_<startedAtMs>.edf
 *   ecg: POST api/v2/ecg/prepare {sessionId, deviceId, horseChipNumber, fileName, startedAt,
 *        endedAt} -> {id, uploadUrl}; PUT the bytes to uploadUrl; POST api/v2/ecg/<id>/process
 *   imu: POST api/imu (same body) -> {id, uploadUrl}; PUT; no process step
 * Up to UPLOAD_ATTEMPTS per file; a file is deleted once every step returned 2xx (as VPro),
 * and its session folder once it is empty. Anything left is retried on the next connection.
 */
typedef struct {
    char path[560];   /* dir + "/" + name (FAT names up to 255) */
    char name[256];
    char prefix[8];
    char device[24];
    char horse[32];
    uint64_t session, started, ended;
    long size;
} upload_file_t;

static bool ok2xx(int status) { return status / 100 == 2; }

static bool parse_file(const char *dir, const char *session_dir, const char *name, upload_file_t *f)
{
    memset(f, 0, sizeof(*f));
    char ext[8];
    unsigned long long started = 0;
    if (sscanf(name, "%7[^_]_%23[^_]_%31[^_]_%llu.%7s", f->prefix, f->device, f->horse,
               &started, ext) != 5 || strcmp(ext, "edf") != 0) {
        return false;
    }
    if (strcmp(f->prefix, "ecg") != 0 && strcmp(f->prefix, "imu") != 0) {
        return false;
    }
    f->started = started;
    f->session = strtoull(session_dir, NULL, 10);
    strlcpy(f->name, name, sizeof(f->name));
    snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
    if (strcmp(f->horse, "Unknown") == 0) {
        f->horse[0] = 0;
    }
    FILE *fp = fopen(f->path, "rb");
    if (!fp) {
        return false;
    }
    /* EDF header: number of data records at 236, record duration (s) at 244, 8 ASCII each.
     * endedAt = startedAt + records x duration (VPro: startedAt + the file duration). */
    char hdr[256];
    if (fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr)) {
        char nrec[9] = { 0 }, dur[9] = { 0 };
        memcpy(nrec, hdr + 236, 8);
        memcpy(dur, hdr + 244, 8);
        long n = strtol(nrec, NULL, 10);
        double d = strtod(dur, NULL);
        if (n > 0 && d > 0) {
            f->ended = f->started + (uint64_t)(n * d * 1000.0 + 0.5);
        }
    }
    fseek(fp, 0, SEEK_END);
    f->size = ftell(fp);
    fclose(fp);
    return f->size > 256;
}

/* PUT the file to the pre-signed URL. Returns the HTTP status (< 0: no reply). */
static int put_file(const upload_file_t *f, const char *upload_url)
{
    FILE *fp = fopen(f->path, "rb");
    char *buf = malloc(UPLOAD_CHUNK);
    if (!fp || !buf) {
        if (fp) {
            fclose(fp);
        }
        free(buf);
        return -1;
    }
    esp_http_client_config_t hc = {
        .url = upload_url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size_tx = 8192,     /* the pre-signed URL goes in the request line */
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    int status = -1;
    if (c) {
        /* The URL carries the signature: no API key, no Content-Type (as VPro). */
        esp_http_client_delete_header(c, "Content-Type");
        esp_err_t err = esp_http_client_open(c, f->size);
        if (err == ESP_OK) {
            long sent = 0;
            size_t n;
            int next_pct = 25;
            int64_t t0 = esp_timer_get_time();
            while ((n = fread(buf, 1, UPLOAD_CHUNK, fp)) > 0) {
                if (esp_http_client_write(c, buf, n) != (int)n) {
                    break;
                }
                sent += n;
                if (sent * 100 / f->size >= next_pct) {
                    ESP_LOGI(TAG, "  %s: %ld %%", f->name, sent * 100 / f->size);
                    next_pct += 25;
                }
            }
            if (sent == f->size) {
                esp_http_client_fetch_headers(c);
                status = esp_http_client_get_status_code(c);
                char resp[129];
                int r = esp_http_client_read_response(c, resp, sizeof(resp) - 1);
                resp[r > 0 ? r : 0] = 0;
                ESP_LOGI(TAG, "PUT %ld bytes in %lld ms -> %d %s", sent,
                         (esp_timer_get_time() - t0) / 1000, status, resp);
            } else {
                ESP_LOGW(TAG, "PUT stopped after %ld of %ld bytes", sent, f->size);
            }
        } else {
            ESP_LOGW(TAG, "PUT failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(c);
    }
    fclose(fp);
    free(buf);
    return status;
}

/* One attempt at prepare -> PUT -> process. Returns the last HTTP status. */
static int upload_once(const upload_file_t *f, char *resp)
{
    bool ecg = strcmp(f->prefix, "ecg") == 0;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "sessionId", (double)f->session);
    cJSON_AddStringToObject(j, "deviceId", f->device[0] ? f->device : s_cfg.device_id);
    if (f->horse[0]) {
        cJSON_AddStringToObject(j, "horseChipNumber", f->horse);
    }
    cJSON_AddStringToObject(j, "fileName", f->name);
    if (f->started) {
        cJSON_AddNumberToObject(j, "startedAt", (double)f->started);
    }
    if (f->ended) {
        cJSON_AddNumberToObject(j, "endedAt", (double)f->ended);
    }
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return -2;
    }
    ESP_LOGI(TAG, "prepare: %s", body);
    int status = api_post(ecg ? "api/v2/ecg/prepare" : "api/imu", body, resp, RESP_MAX);
    cJSON_free(body);
    if (!ok2xx(status)) {
        return status;
    }
    cJSON *r = cJSON_Parse(resp);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "id");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(r, "uploadUrl");
    char id_s[64] = { 0 };
    char *url_s = NULL;
    if (cJSON_IsString(id)) {
        strlcpy(id_s, id->valuestring, sizeof(id_s));
    } else if (cJSON_IsNumber(id)) {
        snprintf(id_s, sizeof(id_s), "%.0f", id->valuedouble);
    }
    if (cJSON_IsString(url)) {
        url_s = strdup(url->valuestring);
    }
    cJSON_Delete(r);
    if (!id_s[0] || !url_s) {
        ESP_LOGE(TAG, "prepare: no \"id\" / \"uploadUrl\" in the response");
        free(url_s);
        return -2;
    }
    status = put_file(f, url_s);
    free(url_s);
    if (!ok2xx(status) || !ecg) {
        return status;
    }
    char path[96];
    snprintf(path, sizeof(path), "api/v2/ecg/%s/process", id_s);
    return api_post(path, NULL, NULL, 0);
}

/* Upload every recording on the card (send false: only count them). Returns the number of
 * recordings still there. */
static int walk_files(bool send)
{
    if (!storage_mounted() && storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "no SD card: nothing to upload");
        return 0;
    }
    DIR *root = opendir(STORAGE_MOUNT "/sessions");
    if (!root) {
        return 0;
    }
    char *resp = malloc(RESP_MAX);
    upload_file_t *f = malloc(sizeof(*f));
    if (!resp || !f) {
        free(resp);
        free(f);
        closedir(root);
        return 0;
    }
    int done = 0, left = 0;
    bool offline = false;
    struct dirent *s;
    while (!offline && (s = readdir(root)) != NULL) {
        if (s->d_type != DT_DIR || s->d_name[0] == '.') {
            continue;
        }
        char dir[300];
        snprintf(dir, sizeof(dir), STORAGE_MOUNT "/sessions/%s", s->d_name);
        DIR *d = opendir(dir);
        if (!d) {
            continue;
        }
        struct dirent *e;
        while (!offline && (e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) {
                continue;
            }
            if (!parse_file(dir, s->d_name, e->d_name, f)) {
                ESP_LOGW(TAG, "skipping %s/%s (not a recording, or empty)", s->d_name, e->d_name);
                continue;
            }
            if (!send) {
                left++;
                continue;
            }
            ESP_LOGI(TAG, "uploading %s (%ld bytes)", f->path, f->size);
            int status = -1;
            for (int a = 1; a <= UPLOAD_ATTEMPTS; a++) {
                status = upload_once(f, resp);
                if (ok2xx(status) || status == -1) {
                    break;   /* done, or no reply at all (Wi-Fi gone): stop for now */
                }
                ESP_LOGW(TAG, "attempt %d of %d failed (%d)", a, UPLOAD_ATTEMPTS, status);
                vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_MS));
            }
            if (ok2xx(status)) {
                unlink(f->path);
                ESP_LOGI(TAG, "uploaded and deleted %s", f->name);
                done++;
            } else {
                left++;
                offline = status == -1;
            }
        }
        closedir(d);
        if (send && rmdir(dir) == 0) {   /* only succeeds once the folder is empty */
            ESP_LOGI(TAG, "session folder %s done", s->d_name);
        }
    }
    closedir(root);
    free(resp);
    free(f);
    if (send && (done || left)) {
        ESP_LOGI(TAG, "uploads: %d sent, %d left%s", done, left, offline ? " (connection lost)" : "");
    }
    return left;
}

static int upload_files(void) { return walk_files(true); }
static int pending_files(void) { return walk_files(false); }

/* ------------------------------------------------------------------- task */

static void show(cloud_show_t state)
{
    if (s_cfg.show) {
        s_cfg.show(state);
    }
}

/* Wait after a failed round: 30 s, 60 s, 120 s, then 300 s. */
static uint32_t backoff_s(int fails)
{
    static const uint32_t steps[] = { 30, 60, 120, 300 };
    int i = fails - 1;
    return steps[i < 0 ? 0 : i > 3 ? 3 : i];
}

static void cloud_task(void *arg)
{
    bool sent_full = false;
    int fails = 0;
    if (strstr(API_DATA_KEY, "PASTE_") || !API_DATA_URL[0]) {
        ESP_LOGE(TAG, "private/api_env.h has no data key: cloud calls disabled");
        vTaskDelete(NULL);
    }
    if (prov_network_count() == 0) {
        ESP_LOGW(TAG, "no saved Wi-Fi network: cloud calls disabled");
        show(CLOUD_SHOW_NONE);
        vTaskDelete(NULL);
    }
    if (wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed");
        show(CLOUD_SHOW_FAILED);
        vTaskDelete(NULL);
    }
    for (;;) {
        cloud_status_t st = { .charging = true };
        if (s_cfg.read_status) {
            s_cfg.read_status(&st);
        }
        int pending = pending_files();
        if (!st.charging && sent_full && pending == 0) {
            ESP_LOGI(TAG, "battery full and reported, no recordings left: cloud task done");
            break;
        }
        if (pending) {
            ESP_LOGI(TAG, "%d recording(s) waiting to upload", pending);
        }
        show(CLOUD_SHOW_CONNECTING);
        bool ok = false;
        int left = pending;
        if (!wifi_connect()) {
            ESP_LOGW(TAG, "no saved network reachable");
        } else {
            time_sync();
            if (time(NULL) < 1704067200) {   /* before 2024: certificates can't be checked */
                ESP_LOGW(TAG, "no valid time: skipping HTTPS");
            } else {
                int status = send_charging(&st);
                if (status / 100 == 2 && !st.charging) {
                    sent_full = true;
                }
                if (pending) {
                    show(CLOUD_SHOW_UPLOADING);
                    left = upload_files();
                }
                ok = status / 100 == 2;
            }
            wifi_off();
        }
        uint32_t wait_s;
        if (!ok) {
            fails++;
            wait_s = backoff_s(fails);
            show(CLOUD_SHOW_FAILED);
        } else if (left) {
            fails = 0;
            wait_s = RETRY_PENDING_S;
            show(CLOUD_SHOW_FAILED);         /* connected, but not everything went up */
        } else {
            fails = 0;
            wait_s = CLOUD_PERIOD_S;
            show(CLOUD_SHOW_SYNCED);
        }
        ESP_LOGI(TAG, "next attempt in %lu s", (unsigned long)wait_s);
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
    vTaskDelete(NULL);
}

esp_err_t cloud_charging_start(const cloud_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_ev = xEventGroupCreate();
    if (!s_ev) {
        return ESP_ERR_NO_MEM;
    }
    /* TLS + FAT need a fair stack. */
    return xTaskCreate(cloud_task, "cloud", 10240, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
