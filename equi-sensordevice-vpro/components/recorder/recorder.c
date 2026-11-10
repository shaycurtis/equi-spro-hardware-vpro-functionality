#include "recorder.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ecg.h"
#include "sensors.h"
#include "storage.h"

static const char *TAG = "recorder";

#define RING_SECONDS   120                  /* PSRAM buffer per signal: 4x the flush period */
#define CHUNK_SAMPLES  1024                 /* staging buffer for one fwrite */
#define MAX_SIGNALS    9
#define OFF_NRECORDS   236
#define STARVE_RECORDS 10                   /* a signal this far behind the others is padded */
#define TAL_SAMPLES    60                   /* EDF Annotations signal: 120 bytes per record (as VPro) */

/* ------------------------------------------------------------ data model */

typedef struct {
    const char *label, *transducer, *dim, *phys_min, *phys_max, *prefilter;
    uint16_t spr;                           /* samples per data record */
    float rate;                             /* real samples per second */
    int16_t *ring;
    uint32_t ring_len;
    volatile uint32_t head;                 /* samples pushed (producer task) */
    uint32_t tail;                          /* samples written (writer task) */
    int64_t head_us;                        /* esp_timer time of the latest push */
    uint32_t switch_at;
    int16_t last;
} sig_t;

typedef struct {
    const char *type;                       /* file name prefix: "ecg", "imu" */
    int nsig;
    sig_t sig[MAX_SIGNALS];
    char duration[9];                       /* EDF record duration, seconds */
    volatile bool ready;                    /* buffers allocated: producer may push */
    bool started;                           /* first sample received (aligned to s_t0) */
    FILE *f;
    char path[112];
    int16_t *chunk;                         /* staging buffer for fwrite (one per writer task) */
    int64_t t0;                             /* esp_timer time of sample 0 (the first real one) */
    int64_t t0_epoch_ms;                    /* the same in Unix ms, 0 = clock not set */
    uint32_t hdr_epoch;                     /* this file's header start (Unix s), 0 = unknown */
    uint32_t file_start;                    /* first sample of this file (signal 0's index) */
    double rec_s;                           /* record duration, seconds */
    double onset0;                          /* first record's onset after the header start, s */
    bool onset_set;
    uint32_t records;
    volatile uint32_t dropped;
    bool starving;                          /* warned about a signal that stopped */
} stream_t;

typedef struct {
    uint64_t session;                       /* folder */
    uint64_t file_id;                       /* last field of the file name (session start, or
                                             * an app recording's start time), ms */
    uint32_t epoch;
    char horse[16];
} meta_t;

static char s_dev_id[24];
static stream_t s_ecg, s_imu;
static stream_t *s_streams[2];
static int s_nstreams;
static int64_t s_t0;                        /* session start (esp_timer): sample 0 of every stream */

static SemaphoreHandle_t s_lock;            /* guards the switch request */
static meta_t s_cur, s_next;
static bool s_switch_pending;
static volatile bool s_stopping, s_closed;
static TaskHandle_t s_task;
static int16_t *s_chunk;

/* ------------------------------------------------------------- producers */

/* Append samples to one signal. The first push pads the signal with its first value back to
 * the session start, so every file shares the same time zero. */
static void sig_push(stream_t *st, sig_t *s, const int16_t *v, size_t n, size_t stride, int64_t first_us)
{
    if (n == 0) {
        return;
    }
    uint32_t head = s->head;
    uint32_t pad = 0;
    if (head == 0 && first_us > st->t0) {
        pad = (uint32_t)lroundf((first_us - st->t0) / 1e6f * s->rate);
    }
    uint32_t tail = __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE);
    if (head - tail + pad + n > s->ring_len) {
        st->dropped += n;                   /* writer stalled: drop rather than overwrite */
        return;
    }
    for (uint32_t i = 0; i < pad; i++) {
        s->ring[(head + i) % s->ring_len] = v[0];
    }
    head += pad;
    for (size_t i = 0; i < n; i++) {
        s->ring[(head + i) % s->ring_len] = v[i * stride];
    }
    s->head_us = esp_timer_get_time();
    __atomic_store_n(&s->head, head + n, __ATOMIC_RELEASE);
}

/* Unix ms of an esp_timer time, 0 if the clock is not set. */
static int64_t epoch_ms_at(int64_t us)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1704067200) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 - (esp_timer_get_time() - us) / 1000;
}

/* The first batch of a stream: it starts here (EDF+ gives each file's exact start). */
static void stream_first(stream_t *st, int64_t first_us)
{
    st->t0 = first_us;
    st->t0_epoch_ms = epoch_ms_at(first_us);
    st->started = true;
}

static void ecg_push(const int16_t *v, size_t n, void *ctx)
{
    stream_t *st = ctx;   /* the session's ECG file, or an app recording */
    if (!st->ready) {
        return;
    }
    int64_t first_us = 0;
    if (!st->started) {
        /* the newest sample was converted about now */
        first_us = esp_timer_get_time() - (int64_t)((n - 1) * 1e6f / st->sig[0].rate);
        stream_first(st, first_us);
    }
    sig_push(st, &st->sig[0], v, n, 1, first_us);
}

static void imu_push(const imu_batch_t *b, void *ctx)
{
    stream_t *st = &s_imu;
    if (!st->ready) {
        return;   /* header not set up yet (the sensor had to start first) */
    }
    int64_t first_us = 0;
    if (!st->started) {
        first_us = b->first_us ? b->first_us : esp_timer_get_time();
        stream_first(st, first_us);
    }
    for (int a = 0; a < 3; a++) {
        sig_push(st, &st->sig[a], &b->acc[0][a], b->n_acc, 3, first_us);
        sig_push(st, &st->sig[3 + a], &b->gyr[0][a], b->n_gyr, 3, first_us);
        if (st->nsig == 9) {
            sig_push(st, &st->sig[6 + a], &b->mag[0][a], b->n_mag, 3, first_us);
        }
    }
}

/* ------------------------------------------------------------------- EDF */

static void field(char *dst, size_t len, const char *src)
{
    size_t n = strlen(src);
    memset(dst, ' ', len);
    memcpy(dst, src, n < len ? n : len);
}

/*
 * EDF+C, as the VPro firmware writes (edflib): EDF+ patient / recording subfields and one
 * "EDF Annotations" signal whose time-keeping TAL gives each record's onset, so the file's
 * start is exact to the sample although the header start time has 1 s resolution.
 */
static void write_header(stream_t *st, const meta_t *m)
{
    static const char *MON[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    int nd = st->nsig;            /* data signals */
    int ns = nd + 1;              /* + EDF Annotations */
    size_t len = 256 * (size_t)(ns + 1);
    char *h = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!h) {
        return;
    }
    char buf[96];
    memset(h, ' ', len);
    field(h + 0, 8, "0");
    /* EDF+ patient: code sex birthdate name (X = unknown); code = the chip number */
    snprintf(buf, sizeof(buf), "%s X X X", m->horse[0] ? m->horse : "Unknown");
    field(h + 8, 80, buf);
    struct tm t = { .tm_mday = 1, .tm_mon = 0, .tm_year = 85 };   /* EDF "unknown": 01.01.85 */
    if (m->epoch) {
        time_t e = m->epoch;
        gmtime_r(&e, &t);
        /* EDF+ recording: Startdate dd-MMM-yyyy admincode technician equipment */
        snprintf(buf, sizeof(buf), "Startdate %02d-%s-%04d X X X", t.tm_mday, MON[t.tm_mon], t.tm_year + 1900);
    } else {
        strlcpy(buf, "Startdate X X X X", sizeof(buf));
    }
    field(h + 88, 80, buf);
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d", t.tm_mday, t.tm_mon + 1, t.tm_year % 100);
    field(h + 168, 8, buf);
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d", t.tm_hour, t.tm_min, t.tm_sec);
    field(h + 176, 8, buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)len);
    field(h + 184, 8, buf);
    field(h + 192, 44, "EDF+C");
    field(h + OFF_NRECORDS, 8, "-1");      /* "unknown" until the first flush */
    field(h + 244, 8, st->duration);
    snprintf(buf, sizeof(buf), "%d", ns);
    field(h + 252, 4, buf);

    /* Signal fields are stored field by field: all labels, then all transducers, ...
     * The last signal is EDF Annotations (blank text fields, -1..1, TAL_SAMPLES). */
#define SIGF(expr, ann) (i < nd ? (expr) : (ann))
    char *p = h + 256;
    for (int i = 0; i < ns; i++) field(p + i * 16, 16, SIGF(st->sig[i].label, "EDF Annotations"));
    p += 16 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 80, 80, SIGF(st->sig[i].transducer, ""));
    p += 80 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 8, 8, SIGF(st->sig[i].dim, ""));
    p += 8 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 8, 8, SIGF(st->sig[i].phys_min, "-1"));
    p += 8 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 8, 8, SIGF(st->sig[i].phys_max, "1"));
    p += 8 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 8, 8, "-32768");
    p += 8 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 8, 8, "32767");
    p += 8 * ns;
    for (int i = 0; i < ns; i++) field(p + i * 80, 80, SIGF(st->sig[i].prefilter, ""));
    p += 80 * ns;
    for (int i = 0; i < ns; i++) {
        snprintf(buf, sizeof(buf), "%u", i < nd ? st->sig[i].spr : TAL_SAMPLES);
        field(p + i * 8, 8, buf);
    }
#undef SIGF
    /* the remaining 32 bytes per signal are reserved (spaces) */
    fwrite(h, 1, len, st->f);
    free(h);
}

/* The record's time-keeping TAL ("+<onset>" 0x14 0x14 0x00, zero padded), as edflib writes it. */
static void write_tal(stream_t *st)
{
    if (!st->onset_set) {
        /* This file's first sample, relative to the start second in its header. */
        double onset = 0;
        if (st->t0_epoch_ms && st->hdr_epoch) {
            double first_ms = st->t0_epoch_ms + st->file_start * 1000.0 / st->sig[0].rate;
            onset = (first_ms - st->hdr_epoch * 1000.0) / 1000.0;
            if (onset < 0) {
                onset = 0;
            }
        }
        st->onset0 = onset;
        st->onset_set = true;
    }
    char tal[TAL_SAMPLES * 2] = { 0 };
    int n = snprintf(tal, sizeof(tal), "+%.7f", st->onset0 + st->records * st->rec_s);
    if (n > 0 && n + 3 <= (int)sizeof(tal)) {
        tal[n] = 0x14;
        tal[n + 1] = 0x14;   /* empty annotation list; the rest stays 0 */
    }
    if (st->f) {
        fwrite(tal, 1, sizeof(tal), st->f);
    }
}

static void open_file(stream_t *st, const meta_t *m)
{
    char *path = st->path;
    mkdir(STORAGE_MOUNT "/sessions", 0777);                       /* EEXIST is fine */
    int n = snprintf(path, sizeof(st->path), STORAGE_MOUNT "/sessions/%llu", (unsigned long long)m->session);
    mkdir(path, 0777);
    snprintf(path + n, sizeof(st->path) - n, "/%s_%s_%s_%llu.edf", st->type, s_dev_id,
             m->horse[0] ? m->horse : "Unknown", (unsigned long long)m->file_id);
    st->f = fopen(path, "wb");
    if (!st->f) {
        ESP_LOGE(TAG, "cannot create %s", path);
        return;
    }
    setvbuf(st->f, NULL, _IOFBF, 8192);
    st->hdr_epoch = m->epoch;
    st->file_start = st->sig[0].tail;
    st->rec_s = strtod(st->duration, NULL);
    st->onset_set = false;
    write_header(st, m);
    fflush(st->f);
    st->records = 0;
    ESP_LOGI(TAG, "recording to %s", path);
}

static void sync_file(stream_t *st)
{
    if (!st->f) {
        return;
    }
    char buf[9];
    snprintf(buf, sizeof(buf), "%-8lu", (unsigned long)st->records);
    long end = ftell(st->f);
    fseek(st->f, OFF_NRECORDS, SEEK_SET);
    fwrite(buf, 1, 8, st->f);
    fseek(st->f, end, SEEK_SET);
    fflush(st->f);
    fsync(fileno(st->f));
}

static void close_file(stream_t *st)
{
    if (st->f) {
        sync_file(st);
        fclose(st->f);
        st->f = NULL;
        ESP_LOGI(TAG, "closed the %s file: %lu records", st->type, (unsigned long)st->records);
    }
}

/* Write `count` samples of one signal: real ones from the ring up to `limit`, then the last
 * value repeated (padding of the final record). */
static void write_sig(stream_t *st, sig_t *s, uint32_t count, uint32_t limit)
{
    while (count) {
        uint32_t n = count < CHUNK_SAMPLES ? count : CHUNK_SAMPLES;
        uint32_t avail = limit - s->tail;
        uint32_t used = n < avail ? n : avail;
        for (uint32_t i = 0; i < n; i++) {
            if (i < used) {
                s->last = s->ring[(s->tail + i) % s->ring_len];
            }
            st->chunk[i] = s->last;
        }
        __atomic_store_n(&s->tail, s->tail + used, __ATOMIC_RELEASE);
        if (st->f) {
            fwrite(st->chunk, sizeof(int16_t), n, st->f);   /* little-endian, as EDF wants */
        }
        count -= n;
    }
}

/* Write whole records while every signal has one (up to each signal's limit). With pad, finish
 * the last partial records too, so every signal ends together (the longest one decides). */
static void write_records(stream_t *st, const uint32_t *limit, bool pad)
{
    uint32_t nrec = pad ? 0 : UINT32_MAX, most = 0;
    for (int i = 0; i < st->nsig; i++) {
        sig_t *s = &st->sig[i];
        uint32_t avail = limit[i] - s->tail;
        uint32_t r = pad ? (avail + s->spr - 1) / s->spr : avail / s->spr;
        nrec = pad ? (r > nrec ? r : nrec) : (r < nrec ? r : nrec);
        most = r > most ? r : most;
    }
    if (!pad && most > nrec + STARVE_RECORDS) {
        /* A signal has stopped delivering (e.g. a sensor failed): keep the file going and pad
         * that signal with its last value, instead of holding everything until the buffer fills. */
        if (!st->starving) {
            ESP_LOGW(TAG, "%s: a signal is %lu records behind; padding it", st->type,
                     (unsigned long)(most - nrec));
            st->starving = true;
        }
        nrec = most - STARVE_RECORDS;
    }
    for (uint32_t r = 0; r < nrec; r++) {
        for (int i = 0; i < st->nsig; i++) {
            write_sig(st, &st->sig[i], st->sig[i].spr, limit[i]);
        }
        write_tal(st);
        st->records++;
    }
}

/* ---------------------------------------------------------------- writer */

static bool switch_ready(void)
{
    for (int k = 0; k < s_nstreams; k++) {
        for (int i = 0; i < s_streams[k]->nsig; i++) {
            sig_t *s = &s_streams[k]->sig[i];
            if ((int32_t)(__atomic_load_n(&s->head, __ATOMIC_ACQUIRE) - s->switch_at) < 0) {
                return false;
            }
        }
    }
    return true;
}

static void writer_task(void *arg)
{
    uint32_t lim[MAX_SIGNALS];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RECORDER_FLUSH_S * 1000));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool switching = s_switch_pending;
        meta_t next = s_next;
        xSemaphoreGive(s_lock);

        if (switching) {
            if (!switch_ready() && !s_stopping) {
                /* a sensor still holds samples from before the switch in its FIFO */
                vTaskDelay(pdMS_TO_TICKS(100));
                xTaskNotifyGive(xTaskGetCurrentTaskHandle());
                continue;
            }
            for (int k = 0; k < s_nstreams; k++) {
                stream_t *st = s_streams[k];
                for (int i = 0; i < st->nsig; i++) {
                    uint32_t head = __atomic_load_n(&st->sig[i].head, __ATOMIC_ACQUIRE);
                    lim[i] = (int32_t)(head - st->sig[i].switch_at) >= 0 ? st->sig[i].switch_at : head;
                }
                write_records(st, lim, true);
                close_file(st);
                open_file(st, &next);
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_cur = next;
            s_switch_pending = false;
            xSemaphoreGive(s_lock);
        }
        for (int k = 0; k < s_nstreams; k++) {
            stream_t *st = s_streams[k];
            for (int i = 0; i < st->nsig; i++) {
                lim[i] = __atomic_load_n(&st->sig[i].head, __ATOMIC_ACQUIRE);
            }
            write_records(st, lim, s_stopping);
            if (s_stopping) {
                close_file(st);
            } else {
                sync_file(st);
            }
            if (st->dropped) {
                ESP_LOGW(TAG, "%s: %lu samples dropped (card too slow?)", st->type, (unsigned long)st->dropped);
                st->dropped = 0;
            }
        }
        if (s_stopping) {
            s_closed = true;
            vTaskDelete(NULL);
        }
    }
}

/* ------------------------------------------------------------- set-up */

static esp_err_t sig_alloc(sig_t *s)
{
    s->ring_len = (uint32_t)ceilf(s->rate) * RING_SECONDS;
    s->ring = heap_caps_malloc(s->ring_len * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    return s->ring ? ESP_OK : ESP_ERR_NO_MEM;
}

static sig_t mk_sig(const char *label, const char *transducer, const char *dim, const char *pmin,
                    const char *pmax, const char *prefilter, uint16_t spr, float rate)
{
    return (sig_t){ .label = label, .transducer = transducer, .dim = dim, .phys_min = pmin,
                    .phys_max = pmax, .prefilter = prefilter, .spr = spr, .rate = rate };
}

static esp_err_t setup_ecg(stream_t *st, uint16_t rate)
{
    *st = (stream_t){ .type = "ecg", .nsig = 1 };
    /* int16 = 18-bit sample >> 2: 0.3814697265625 uV/LSB, so -32768..32767 = -12.5..12.49962 mV
     * (mV as the VPro platform's ECG files, which its backend processing reads) */
    st->sig[0] = mk_sig("ECG", "Electrodes (MAX30001, gain 80)", "mV", "-12.5", "12.49962",
                          "HP:0.5Hz LP:40Hz", rate / 2, rate);
    strlcpy(st->duration, "0.5", sizeof(st->duration));   /* 0.5 s records, as VPro */
    return sig_alloc(&st->sig[0]);
}

static esp_err_t setup_imu(uint16_t rate, float actual, bool mag)
{
    static const char *acc[] = { "AccX", "AccY", "AccZ" };
    static const char *gyr[] = { "GyrX", "GyrY", "GyrZ" };
    static const char *mg[] = { "MagX", "MagY", "MagZ" };
    stream_t *st = &s_imu;
    *st = (stream_t){ .type = "imu", .nsig = mag ? 9 : 6 };
    /* The IMU runs from its own oscillator: a record holds `rate` samples, so it lasts
     * rate/actual seconds of real time (written as the record duration; the mag shares it). */
    float scale = actual / rate;
    for (int a = 0; a < 3; a++) {
        /* 0.488 mg/LSB, 70 mdps/LSB, 1.5 mgauss/LSB = 0.15 uT/LSB */
        st->sig[a] = mk_sig(acc[a], "LSM6DSOX accelerometer +-16 g", "g", "-15.9908", "15.9903", "",
                              rate, actual);
        st->sig[3 + a] = mk_sig(gyr[a], "LSM6DSOX gyroscope +-2000 dps", "dps", "-2293.76", "2293.69", "",
                                  rate, actual);
        if (mag) {
            st->sig[6 + a] = mk_sig(mg[a], "LIS2MDL magnetometer", "uT", "-4915.2", "4915.05", "",
                                      104, 104 * scale);
        }
    }
    snprintf(st->duration, sizeof(st->duration), "%.6f", rate / actual);
    for (int i = 0; i < st->nsig; i++) {
        esp_err_t err = sig_alloc(&st->sig[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void set_horse(meta_t *m, const char *horse)
{
    if (horse) {
        strlcpy(m->horse, horse, sizeof(m->horse));
    }
}

/* ------------------------------------------------------------------ API */

esp_err_t recorder_start(const recorder_cfg_t *cfg)
{
    if (!storage_mounted() || s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg->ecg_rate_hz && !cfg->imu_rate_hz) {
        return ESP_OK;   /* nothing to record */
    }
    strlcpy(s_dev_id, cfg->device_id ? cfg->device_id : "", sizeof(s_dev_id));
    s_t0 = esp_timer_get_time();
    s_lock = xSemaphoreCreateMutex();
    s_chunk = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_chunk) {
        return ESP_ERR_NO_MEM;
    }
    s_cur = (meta_t){ .session = cfg->session, .file_id = cfg->session, .epoch = cfg->epoch };
    set_horse(&s_cur, cfg->horse);

    if (cfg->ecg_rate_hz && recorder_clip_active()) {
        ESP_LOGI(TAG, "session ECG file takes over the ECG: saving the app recording");
        recorder_clip_stop(true);
        for (int i = 0; i < 60 && recorder_clip_active(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (cfg->ecg_rate_hz && setup_ecg(&s_ecg, cfg->ecg_rate_hz) == ESP_OK) {
        s_streams[s_nstreams++] = &s_ecg;
    }
    if (cfg->imu_rate_hz) {
        /* The sensor starts first: the header needs its real rate and whether the mag answered.
         * imu_push drops batches until the stream is marked ready. */
        float actual = cfg->imu_rate_hz;
        esp_err_t err = sensors_imu_stream_start(cfg->imu_rate_hz, imu_push, NULL, &actual);
        if (err == ESP_OK) {
            err = setup_imu(cfg->imu_rate_hz, actual, sensors_mag_ok());
        }
        if (err == ESP_OK) {
            s_streams[s_nstreams++] = &s_imu;
        } else {
            ESP_LOGW(TAG, "IMU recording not started: %s", esp_err_to_name(err));
            sensors_imu_stream_stop();
        }
    }
    for (int k = 0; k < s_nstreams; k++) {
        s_streams[k]->chunk = s_chunk;
        s_streams[k]->t0 = s_t0;
        open_file(s_streams[k], &s_cur);
        s_streams[k]->ready = true;
    }
    if (xTaskCreate(writer_task, "recorder", 6144, NULL, 2, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (s_ecg.ready) {
        ecg_stream_start(cfg->ecg_rate_hz, ecg_push, &s_ecg);
    }
    return ESP_OK;
}

void recorder_new_session(uint64_t session, uint32_t epoch, const char *horse)
{
    if (!s_task) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Each signal switches at the sample taken now: the last pushed one plus what the sensor
     * has produced since (still waiting in its FIFO). */
    for (int k = 0; k < s_nstreams; k++) {
        for (int i = 0; i < s_streams[k]->nsig; i++) {
            sig_t *s = &s_streams[k]->sig[i];
            uint32_t head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
            int64_t dt = now - s->head_us;
            s->switch_at = head + (head && dt > 0 ? (uint32_t)lroundf(dt / 1e6f * s->rate) : 0);
        }
    }
    s_next = (meta_t){ .session = session, .file_id = session, .epoch = epoch };
    set_horse(&s_next, horse);
    s_switch_pending = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void recorder_shutdown(void)
{
    if (recorder_clip_active()) {
        recorder_clip_stop(true);   /* power-off saves an app recording, as stop would */
        for (int i = 0; i < 80 && recorder_clip_active(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (!s_task) {
        return;
    }
    ecg_stream_stop();
    sensors_imu_stream_stop();
    s_stopping = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 80 && !s_closed; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_closed) {
        ESP_LOGW(TAG, "files not closed in time");
    }
}

/* ------------------------------------------------- app-started recordings */

/*
 * One ECG file per recording the app starts (ECG control service), when the session is not
 * already recording ECG continuously. Its own stream, buffer and writer task, so it works with
 * or without a session recording (e.g. IMU only).
 */
#define CLIP_FLUSH_MS 5000

static stream_t s_clip;
static TaskHandle_t volatile s_clip_task;
static volatile bool s_clip_stop, s_clip_keep;

static void clip_task(void *arg)
{
    stream_t *st = &s_clip;
    uint32_t lim[1];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CLIP_FLUSH_MS));
        bool stop = s_clip_stop;
        if (stop) {
            vTaskDelay(pdMS_TO_TICKS(50));   /* let a push already under way finish */
        }
        lim[0] = __atomic_load_n(&st->sig[0].head, __ATOMIC_ACQUIRE);
        write_records(st, lim, stop);
        if (!stop) {
            sync_file(st);
            continue;
        }
        close_file(st);
        if (!s_clip_keep) {
            unlink(st->path);
            char *slash = strrchr(st->path, '/');
            if (slash) {
                *slash = 0;
                rmdir(st->path);   /* only if the folder is now empty */
            }
            ESP_LOGI(TAG, "app recording discarded");
        }
        if (st->dropped) {
            ESP_LOGW(TAG, "app recording: %lu samples dropped", (unsigned long)st->dropped);
        }
        break;
    }
    free(st->sig[0].ring);
    free(st->chunk);
    st->sig[0].ring = NULL;
    st->chunk = NULL;
    s_clip_task = NULL;
    vTaskDelete(NULL);
}

bool recorder_clip_active(void)
{
    return s_clip_task != NULL;
}

esp_err_t recorder_clip_start(const recorder_clip_cfg_t *c)
{
    if (!storage_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_clip_task || s_ecg.ready) {
        return ESP_ERR_INVALID_STATE;   /* one at a time; or the session file has the ECG */
    }
    if (setup_ecg(&s_clip, c->rate_hz) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    s_clip.chunk = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_clip.chunk) {
        free(s_clip.sig[0].ring);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_dev_id, c->device_id ? c->device_id : "", sizeof(s_dev_id));
    meta_t m = { .session = c->session, .file_id = c->id, .epoch = c->epoch };
    set_horse(&m, c->horse);
    s_clip.t0 = esp_timer_get_time();
    open_file(&s_clip, &m);
    if (!s_clip.f) {
        free(s_clip.sig[0].ring);
        free(s_clip.chunk);
        return ESP_FAIL;
    }
    s_clip_stop = false;
    s_clip_keep = true;
    s_clip.ready = true;
    TaskHandle_t t;
    if (xTaskCreate(clip_task, "ecg_clip", 4096, NULL, 2, &t) != pdPASS) {
        fclose(s_clip.f);
        s_clip.f = NULL;
        unlink(s_clip.path);
        free(s_clip.sig[0].ring);
        free(s_clip.chunk);
        s_clip.ready = false;
        return ESP_ERR_NO_MEM;
    }
    s_clip_task = t;
    ecg_stream_start(c->rate_hz, ecg_push, &s_clip);
    return ESP_OK;
}

void recorder_clip_stop(bool keep)
{
    TaskHandle_t t = s_clip_task;
    if (!t || s_clip_stop) {
        return;
    }
    ecg_stream_stop();
    s_clip.ready = false;
    s_clip_keep = keep;
    s_clip_stop = true;
    xTaskNotifyGive(t);
}
