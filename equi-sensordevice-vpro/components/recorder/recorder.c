#include "recorder.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ecg.h"
#include "storage.h"

static const char *TAG = "recorder";

#define RING_SECONDS   120                  /* PSRAM buffer: 4x the flush period */
#define CHUNK_SAMPLES  4096                 /* staging buffer for one fwrite */
#define HDR_BYTES      512                  /* EDF: 256 + 256 per signal (1 signal) */
#define OFF_NRECORDS   236

/* ECG scale: int16 = 18-bit sample >> 2, 1 LSB = ECG_UV_PER_LSB uV (0.3814697265625). */
#define PHYS_MIN       "-12500"             /* -32768 LSB */
#define PHYS_MAX       "12499.62"           /* +32767 LSB */

typedef struct {
    uint32_t session;
    uint32_t epoch;
    char horse[16];
} meta_t;

static uint32_t s_dev_id;
static uint16_t s_rate;
static int16_t *s_ring;
static uint32_t s_ring_len;
static volatile uint32_t s_head;            /* samples pushed (ECG task) */
static uint32_t s_tail;                     /* samples written (writer task) */
static volatile uint32_t s_dropped;

static SemaphoreHandle_t s_lock;            /* guards everything below */
static meta_t s_cur;
static bool s_switch_pending;
static uint32_t s_switch_at;
static meta_t s_next;
static volatile bool s_stopping;
static volatile bool s_closed;

static TaskHandle_t s_task;
static FILE *s_f;
static uint32_t s_records;
static int16_t s_last;
static int16_t *s_chunk;

/* ------------------------------------------------------------- ECG side */

static void push(const int16_t *v, size_t n, void *ctx)
{
    uint32_t head = s_head;
    uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    if (head - tail + n > s_ring_len) {
        s_dropped += n;                     /* writer stalled: drop rather than overwrite */
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_ring[(head + i) % s_ring_len] = v[i];
    }
    __atomic_store_n(&s_head, head + n, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ EDF */

static void field(char *dst, size_t len, const char *src)
{
    size_t n = strlen(src);
    memset(dst, ' ', len);
    memcpy(dst, src, n < len ? n : len);
}

static void patient_text(const meta_t *m, char out[81])
{
    snprintf(out, 81, "HorseID %s", m->horse[0] ? m->horse : "unknown");
}

static void build_header(char *h, const meta_t *m)
{
    char buf[96];
    memset(h, ' ', HDR_BYTES);
    field(h + 0, 8, "0");
    patient_text(m, buf);
    field(h + 8, 80, buf);
    snprintf(buf, sizeof(buf), "Device EQ-%06lX Session %lu VPro", (unsigned long)s_dev_id,
             (unsigned long)m->session);
    field(h + 88, 80, buf);
    struct tm t = { .tm_mday = 1, .tm_mon = 0, .tm_year = 85 };   /* EDF "unknown": 01.01.85 */
    if (m->epoch) {
        time_t e = m->epoch;
        gmtime_r(&e, &t);
    }
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d", t.tm_mday, t.tm_mon + 1, t.tm_year % 100);
    field(h + 168, 8, buf);
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d", t.tm_hour, t.tm_min, t.tm_sec);
    field(h + 176, 8, buf);
    field(h + 184, 8, "512");
    field(h + 192, 44, "");
    field(h + OFF_NRECORDS, 8, "-1");     /* "unknown" until the first flush */
    field(h + 244, 8, "1");
    field(h + 252, 4, "1");
    /* signal 1 */
    field(h + 256, 16, "ECG");
    field(h + 272, 80, "Electrodes (MAX30001, gain 80)");
    field(h + 352, 8, "uV");
    field(h + 360, 8, PHYS_MIN);
    field(h + 368, 8, PHYS_MAX);
    field(h + 376, 8, "-32768");
    field(h + 384, 8, "32767");
    field(h + 392, 80, "HP:0.5Hz LP:40Hz");
    snprintf(buf, sizeof(buf), "%u", s_rate);
    field(h + 472, 8, buf);
    field(h + 480, 32, "");
}

static void put_at(long off, const char *data, size_t len)
{
    long end = ftell(s_f);
    fseek(s_f, off, SEEK_SET);
    fwrite(data, 1, len, s_f);
    fseek(s_f, end, SEEK_SET);
}

static void open_file(const meta_t *m)
{
    char path[112];
    mkdir(STORAGE_MOUNT "/sessions", 0777);                       /* EEXIST is fine */
    int n = snprintf(path, sizeof(path), STORAGE_MOUNT "/sessions/%lu", (unsigned long)m->session);
    mkdir(path, 0777);
    snprintf(path + n, sizeof(path) - n, "/ecg_%06lX_%s_%lu.edf", (unsigned long)s_dev_id,
             m->horse[0] ? m->horse : "unknown", (unsigned long)m->session);
    s_f = fopen(path, "wb");
    if (!s_f) {
        ESP_LOGE(TAG, "cannot create %s", path);
        return;
    }
    char hdr[HDR_BYTES];
    build_header(hdr, m);
    fwrite(hdr, 1, HDR_BYTES, s_f);
    fflush(s_f);
    s_records = 0;
    ESP_LOGI(TAG, "recording ECG %u Hz to %s", s_rate, path);
}

/* Write `count` samples from the ring (count may exceed what's buffered only for padding). */
static void write_samples(uint32_t count)
{
    while (count) {
        uint32_t n = count < CHUNK_SAMPLES ? count : CHUNK_SAMPLES;
        uint32_t avail = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE) - s_tail;
        for (uint32_t i = 0; i < n; i++) {
            if (i < avail) {
                s_last = s_ring[(s_tail + i) % s_ring_len];
            }
            s_chunk[i] = s_last;              /* past the data: pad with the last value */
        }
        uint32_t used = n < avail ? n : avail;
        __atomic_store_n(&s_tail, s_tail + used, __ATOMIC_RELEASE);
        if (s_f) {
            fwrite(s_chunk, sizeof(int16_t), n, s_f);   /* little-endian, as EDF wants */
        }
        count -= n;
    }
}

static void sync_file(void)
{
    if (!s_f) {
        return;
    }
    char buf[9];
    snprintf(buf, sizeof(buf), "%-8lu", (unsigned long)s_records);
    put_at(OFF_NRECORDS, buf, 8);
    fflush(s_f);
    fsync(fileno(s_f));
}

/* Write whole records up to sample `limit`; pad = also finish a partial last record. */
static void write_records_until(uint32_t limit, bool pad)
{
    uint32_t avail = limit - s_tail;
    uint32_t whole = avail / s_rate;
    if (whole) {
        write_samples(whole * s_rate);
        s_records += whole;
    }
    if (pad && limit != s_tail) {
        /* Finish the last record: its real samples, then the last value repeated. The padding
         * is written directly, so samples past `limit` (the next session's) stay in the ring. */
        uint32_t partial = limit - s_tail;
        write_samples(partial);
        uint32_t fill = s_rate - partial;      /* < s_rate <= CHUNK_SAMPLES */
        for (uint32_t i = 0; i < fill; i++) {
            s_chunk[i] = s_last;
        }
        if (s_f) {
            fwrite(s_chunk, sizeof(int16_t), fill, s_f);
        }
        s_records++;
    }
}

static void close_file(void)
{
    if (s_f) {
        sync_file();
        fclose(s_f);
        s_f = NULL;
        ESP_LOGI(TAG, "closed the ECG file: %lu s", (unsigned long)s_records);
    }
}

/* --------------------------------------------------------------- writer */

static void writer_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RECORDER_FLUSH_S * 1000));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool switching = s_switch_pending;
        uint32_t switch_at = s_switch_at;
        meta_t next = s_next;
        xSemaphoreGive(s_lock);

        if (switching) {
            write_records_until(switch_at, true);
            close_file();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_cur = next;
            s_switch_pending = false;
            xSemaphoreGive(s_lock);
            open_file(&next);
        }
        if (s_stopping) {
            write_records_until(__atomic_load_n(&s_head, __ATOMIC_ACQUIRE), true);
            close_file();
            s_closed = true;
            vTaskDelete(NULL);
        }
        write_records_until(__atomic_load_n(&s_head, __ATOMIC_ACQUIRE), false);
        sync_file();
        if (s_dropped) {
            ESP_LOGW(TAG, "%lu samples dropped (card too slow?)", (unsigned long)s_dropped);
            s_dropped = 0;
        }
    }
}

/* ----------------------------------------------------------------- API */

static void set_horse(meta_t *m, const char *horse)
{
    if (horse) {
        strlcpy(m->horse, horse, sizeof(m->horse));
    }
}

esp_err_t recorder_start(uint32_t device_id, uint32_t session, uint32_t epoch, uint16_t rate_hz,
                         const char *horse)
{
    if (!storage_mounted() || s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_dev_id = device_id;
    s_rate = rate_hz;
    s_ring_len = (uint32_t)rate_hz * RING_SECONDS;
    s_ring = heap_caps_malloc(s_ring_len * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_chunk = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_chunk || !s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_cur = (meta_t){ .session = session, .epoch = epoch };
    set_horse(&s_cur, horse);
    open_file(&s_cur);
    if (!s_f) {
        return ESP_FAIL;
    }
    if (xTaskCreate(writer_task, "recorder", 6144, NULL, 2, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ecg_stream_start(rate_hz, push, NULL);
}

void recorder_new_session(uint32_t session, uint32_t epoch, const char *horse)
{
    if (!s_task) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_switch_at = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);   /* this sample starts the new file */
    s_next = (meta_t){ .session = session, .epoch = epoch };
    set_horse(&s_next, horse);
    s_switch_pending = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void recorder_shutdown(void)
{
    if (!s_task) {
        return;
    }
    ecg_stream_stop();
    s_stopping = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 60 && !s_closed; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_closed) {
        ESP_LOGW(TAG, "file not closed in time");
    }
}
