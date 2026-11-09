#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"

static const char *TAG = "storage";

#define SD_HOST        SPI3_HOST          /* SPI2 belongs to the MAX30001 */
#define SD_FREQ_KHZ    20000              /* SDSPI maximum */
#define CONFIG_PATH    STORAGE_MOUNT "/config.json"
#define CONFIG_VERSION 1

static sdmmc_card_t *s_card;
static storage_config_t s_cfg = {
    .ecg_log = false, .ecg_rate_hz = 256,   /* default: no SD logging (it doubles the drain) */
    .imu_log = false, .imu_rate_hz = 104,
};

bool storage_mounted(void) { return s_card != NULL; }
const storage_config_t *storage_config(void) { return &s_cfg; }

/* ---------------------------------------------------------------- config */

static uint16_t pick_rate(const cJSON *obj, uint16_t a, uint16_t b, uint16_t dflt)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(obj, "rate_hz");
    if (cJSON_IsNumber(r) && (r->valueint == a || r->valueint == b)) {
        return (uint16_t)r->valueint;
    }
    if (r) {
        ESP_LOGW(TAG, "config: rate_hz must be %u or %u; using %u", a, b, dflt);
    }
    return dflt;
}

static bool pick_bool(const cJSON *obj, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static void write_default_config(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", CONFIG_VERSION);
    cJSON *ecg = cJSON_AddObjectToObject(root, "ecg");
    cJSON_AddBoolToObject(ecg, "log", s_cfg.ecg_log);
    cJSON_AddNumberToObject(ecg, "rate_hz", s_cfg.ecg_rate_hz);
    cJSON *imu = cJSON_AddObjectToObject(root, "imu");
    cJSON_AddBoolToObject(imu, "log", s_cfg.imu_log);
    cJSON_AddNumberToObject(imu, "rate_hz", s_cfg.imu_rate_hz);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    FILE *f = text ? fopen(CONFIG_PATH, "w") : NULL;
    if (f) {
        fputs(text, f);
        fputc('\n', f);
        fclose(f);
        ESP_LOGI(TAG, "wrote default %s", CONFIG_PATH);
    } else {
        ESP_LOGW(TAG, "could not write %s", CONFIG_PATH);
    }
    free(text);
}

static void load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        write_default_config();
        return;
    }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "config: %s is not valid JSON; using defaults", CONFIG_PATH);
        return;
    }
    const cJSON *ecg = cJSON_GetObjectItemCaseSensitive(root, "ecg");
    if (cJSON_IsObject(ecg)) {
        s_cfg.ecg_log = pick_bool(ecg, "log", s_cfg.ecg_log);
        s_cfg.ecg_rate_hz = pick_rate(ecg, 256, 512, s_cfg.ecg_rate_hz);
    }
    const cJSON *imu = cJSON_GetObjectItemCaseSensitive(root, "imu");
    if (cJSON_IsObject(imu)) {
        s_cfg.imu_log = pick_bool(imu, "log", s_cfg.imu_log);
        s_cfg.imu_rate_hz = pick_rate(imu, 104, 416, s_cfg.imu_rate_hz);
    }
    cJSON_Delete(root);
}

/* ----------------------------------------------------------------- mount */

static void list_root(void)
{
    DIR *d = opendir(STORAGE_MOUNT);
    if (!d) {
        return;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < 20) {
        char path[300];
        snprintf(path, sizeof(path), STORAGE_MOUNT "/%s", e->d_name);
        struct stat st;
        bool dir = e->d_type == DT_DIR;
        long size = (!dir && stat(path, &st) == 0) ? (long)st.st_size : 0;
        ESP_LOGI(TAG, "  %s%s %ld", e->d_name, dir ? "/" : "", size);
        count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "root: %d entr%s shown", count, count == 1 ? "y" : "ies");
}

esp_err_t storage_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_PIN_SD_MOSI,
        .miso_io_num = BOARD_PIN_SD_MISO,
        .sclk_io_num = BOARD_PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return err;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_PIN_SD_CS;
    slot.host_id = SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,   /* never wipe a card we cannot read */
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
    };
    int64_t t0 = esp_timer_get_time();
    err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGE(TAG, "no card mounted (%s): using the default configuration", esp_err_to_name(err));
        spi_bus_free(SD_HOST);
        return err;
    }
    ESP_LOGI(TAG, "mounted in %lld ms", (esp_timer_get_time() - t0) / 1000);
    sdmmc_card_print_info(stdout, s_card);

    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(STORAGE_MOUNT, &total, &free_b) == ESP_OK) {
        ESP_LOGI(TAG, "FAT: %llu MB total, %llu MB free", total >> 20, free_b >> 20);
    }
    list_root();
    load_config();
    ESP_LOGI(TAG, "config: ECG %s @ %u Hz, IMU %s @ %u Hz (mag 104 Hz)",
             s_cfg.ecg_log ? "on" : "off", s_cfg.ecg_rate_hz,
             s_cfg.imu_log ? "on" : "off", s_cfg.imu_rate_hz);
    return ESP_OK;
}

/* ------------------------------------------------------------ speed test */

esp_err_t storage_speed_test(void)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t chunk = 32 * 1024, total = 1024 * 1024;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < chunk; i++) {
        buf[i] = (uint8_t)i;
    }
    const char *path = STORAGE_MOUNT "/speed.tmp";
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return ESP_FAIL;
    }
    int64_t t0 = esp_timer_get_time();
    size_t written = 0;
    while (written < total && fwrite(buf, 1, chunk, f) == chunk) {
        written += chunk;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    int64_t tw = esp_timer_get_time() - t0;

    f = fopen(path, "rb");
    size_t read = 0;
    bool ok = f != NULL;
    t0 = esp_timer_get_time();
    while (f && read < written && fread(buf, 1, chunk, f) == chunk) {
        for (size_t i = 0; i < chunk; i += 997) {
            ok &= buf[i] == (uint8_t)i;
        }
        read += chunk;
    }
    int64_t tr = esp_timer_get_time() - t0;
    if (f) {
        fclose(f);
    }
    remove(path);
    free(buf);
    ESP_LOGI(TAG, "speed test: write %u KB in %lld ms (%lld KB/s), read %u KB in %lld ms (%lld KB/s), data %s",
             (unsigned)(written >> 10), tw / 1000, tw ? (int64_t)written * 1000 / tw : 0,
             (unsigned)(read >> 10), tr / 1000, tr ? (int64_t)read * 1000 / tr : 0, ok ? "ok" : "MISMATCH");
    return (written == total && read == total && ok) ? ESP_OK : ESP_FAIL;
}

void storage_shutdown(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT, s_card);
        s_card = NULL;
        spi_bus_free(SD_HOST);
    }
}
