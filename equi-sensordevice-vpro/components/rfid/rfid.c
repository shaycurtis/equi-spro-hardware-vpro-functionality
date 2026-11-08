#include "rfid.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"

#define RFID_UART        UART_NUM_2
#define POWER_UP_MS      200
#define FRAME_GAP_MS     40      /* ~38 byte-times at 9600: a silence this long ends a frame */
#define BUF_MAX          96
#define NATIONAL_MAX     ((1ULL << 38) - 1)

static const char *TAG = "rfid";
#include "esp_pm.h"

/* Held while this driver is active: light sleep would stop its clock mid-transfer. */
static esp_pm_lock_handle_t s_pm_lock;
static void awake(bool on)
{
    if (!s_pm_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, TAG, &s_pm_lock) != ESP_OK) {
        return;   /* power management disabled */
    }
    if (on) {
        esp_pm_lock_acquire(s_pm_lock);
    } else {
        esp_pm_lock_release(s_pm_lock);
    }
}

static volatile bool s_busy, s_cancel;
static int s_timeout_s;
static rfid_cb_t s_cb;
static void *s_ctx;

uint64_t rfid_tag_pack(const rfid_tag_t *t)
{
    return ((uint64_t)(t->country & 0x3FF) << 38) | (t->national & NATIONAL_MAX);
}

void rfid_tag_str(const rfid_tag_t *t, char out[16])
{
    /* country <= 999 and national < 10^12 (38 bits), so this is always exactly 15 digits */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%03u%012" PRIu64, (unsigned)(t->country % 1000), t->national % 1000000000000ULL);
    memcpy(out, tmp, 15);
    out[15] = 0;
}

static void log_raw(const uint8_t *b, int n)
{
    char hex[3 * BUF_MAX + 1], asc[BUF_MAX + 1];
    for (int i = 0; i < n; i++) {
        snprintf(&hex[3 * i], 4, "%02X ", b[i]);
        asc[i] = isprint(b[i]) ? (char)b[i] : '.';
    }
    hex[3 * n] = 0;
    asc[n] = 0;
    ESP_LOGI(TAG, "raw frame (%d bytes): %s| %s", n, hex, asc);
}

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/*
 * JY-LD6900 "free port" protocol (manual appendix 1), sent twice per read:
 *   '$' <type 'F' FDX-B | 'E' EM4100> <15 decimal digits> <BCC: 2 ASCII hex> '#'
 *   BCC = XOR of the 16 bytes from <type> to the last digit.
 * FDX-B: 3-digit country/manufacturer code + 12-digit national ID (as printed on the tag).
 * EM4100: 3-digit customer code + "00" + 10-digit card number.
 * Returns true and sets *bcc_ok for every well-formed frame found in buf.
 */
static bool parse_frame(const uint8_t *b, int n, rfid_tag_t *t, bool *bcc_ok)
{
    for (int i = 0; i + 20 <= n; i++) {
        const uint8_t *f = &b[i];
        if (f[0] != '$' || (f[1] != 'F' && f[1] != 'E') || f[19] != '#') {
            continue;
        }
        bool digits = true;
        uint8_t x = f[1];
        for (int k = 2; k < 17; k++) {
            digits &= isdigit(f[k]) != 0;
            x ^= f[k];
        }
        int hi = hexval(f[17]), lo = hexval(f[18]);
        if (!digits || hi < 0 || lo < 0) {
            continue;
        }
        uint64_t country = 0, national = 0;
        for (int k = 2; k < 5; k++) country = country * 10 + (f[k] - '0');
        for (int k = 5; k < 17; k++) national = national * 10 + (f[k] - '0');
        t->country = (uint16_t)country;
        t->national = national;
        t->fdx_b = (f[1] == 'F');
        *bcc_ok = ((hi << 4) | lo) == x && national <= NATIONAL_MAX;
        return true;
    }
    return false;
}

/* Candidate serial settings. The manual documents 9600 8N1 ASCII (and 19200 8E1 for the
 * Modbus "M" variant). The first capture at 9600 8N1 was garbled, so confirm on hardware:
 * also try inverted line polarity and other rates. */
typedef struct { int baud; uart_parity_t parity; bool inverted; const char *name; } serial_cfg_t;
static const serial_cfg_t k_cfgs[] = {
    { 9600,  UART_PARITY_DISABLE, false, "9600 8N1" },
    { 9600,  UART_PARITY_DISABLE, true,  "9600 8N1 inv" },
    { 19200, UART_PARITY_EVEN,    false, "19200 8E1" },
    { 19200, UART_PARITY_DISABLE, false, "19200 8N1" },
    { 19200, UART_PARITY_DISABLE, true,  "19200 inv" },
    { 4800,  UART_PARITY_DISABLE, false, "4800 8N1" },
    { 38400, UART_PARITY_DISABLE, false, "38400 8N1" },
};
/* Verified on the first unit: tag frames are 9600 8N1 ASCII as documented (tag
 * 999012345678436 read with a valid BCC). Its power-up tuning report is sent at 19200
 * instead (header 02 10 FF 04, then the tuning curve), so at 9600 it shows up as one
 * garbled burst right after power-on; it is logged and ignored. Set to -1 to re-run the
 * serial auto-detection for a different reader. */
static int s_cfg = 0;

static void apply_cfg(int i)
{
    uart_config_t cfg = {
        .baud_rate = k_cfgs[i].baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = k_cfgs[i].parity,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,   /* fixed 40 MHz: APB changes with DFS and would shift the baud rate */
    };
    uart_param_config(RFID_UART, &cfg);
    uart_set_line_inverse(RFID_UART, k_cfgs[i].inverted ? UART_SIGNAL_RXD_INV : UART_SIGNAL_INV_DISABLE);
    uart_flush_input(RFID_UART);
}

/* Power-cycle the reader at each setting and look for its power-up tuning report
 * (0xB5 + 32-byte table + tuning point). Leaves the reader powered on the setting found. */
static void detect_serial(void)
{
    uint8_t b[96];
    for (size_t i = 0; i < sizeof(k_cfgs) / sizeof(k_cfgs[0]) && !s_cancel; i++) {
        gpio_set_level(BOARD_PIN_RFID_EN, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        apply_cfg((int)i);
        gpio_set_level(BOARD_PIN_RFID_EN, 1);
        int n = 0;
        int64_t until = esp_timer_get_time() + 3500000;
        while (n < (int)sizeof(b) && esp_timer_get_time() < until) {
            int r = uart_read_bytes(RFID_UART, b + n, sizeof(b) - n, pdMS_TO_TICKS(100));
            n += r > 0 ? r : 0;
        }
        int at = -1;
        for (int k = 0; k + 34 <= n; k++) {
            if (b[k] == 0xB5) {
                at = k;
                break;
            }
        }
        ESP_LOGI(TAG, "probe %-10s: %2d bytes %s", k_cfgs[i].name, n, at >= 0 ? "-> tuning report found" : "");
        if (n) {
            log_raw(b, n > 40 ? 40 : n);
        }
        if (at >= 0) {
            s_cfg = (int)i;
            ESP_LOGI(TAG, "reader uses %s; antenna tuning point %u of 32", k_cfgs[i].name, b[at + 33]);
            return;
        }
    }
    s_cfg = 0;   /* nothing recognised: fall back to the documented 9600 8N1 */
    ESP_LOGW(TAG, "no tuning report recognised at any setting; using %s", k_cfgs[s_cfg].name);
}

static void rfid_task(void *arg)
{
    awake(true);
    uart_driver_install(RFID_UART, 512, 0, 0, NULL, 0);
    uart_set_pin(RFID_UART, UART_PIN_NO_CHANGE, BOARD_PIN_RFID_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (s_cfg < 0) {
        detect_serial();
    } else {
        apply_cfg(s_cfg);
        gpio_set_level(BOARD_PIN_RFID_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(POWER_UP_MS));
    }
    ESP_LOGI(TAG, "reader powered (%s), waiting up to %d s for a tag", k_cfgs[s_cfg].name, s_timeout_s);

    uint8_t buf[BUF_MAX];
    int n = 0;
    bool have_prev = false, done = false;
    rfid_tag_t prev = { 0 }, tag = { 0 };
    int64_t deadline = esp_timer_get_time() + (int64_t)s_timeout_s * 1000000;

    while (!s_cancel && !done && esp_timer_get_time() < deadline) {
        int r = uart_read_bytes(RFID_UART, buf + n, BUF_MAX - n, pdMS_TO_TICKS(FRAME_GAP_MS));
        if (r > 0) {
            n += r;
            if (n < BUF_MAX) {
                continue;              /* keep collecting until the line goes quiet */
            }
        }
        if (n == 0) {
            continue;
        }
        if (buf[0] == 0xB5 && n >= 34) {
            /* Power-up auto-tuning report: 0xB5 + 32-entry table + selected point. */
            ESP_LOGI(TAG, "antenna auto-tune done, tuning point %u of 32", buf[33]);
            n = 0;
            continue;
        }
        rfid_tag_t t;
        bool verified = false;
        if (parse_frame(buf, n, &t, &verified)) {
            bool repeat = have_prev && t.country == prev.country && t.national == prev.national;
            char s[16];
            rfid_tag_str(&t, s);
            ESP_LOGI(TAG, "%s tag %s (%s)", t.fdx_b ? "FDX-B" : "EM4100", s,
                     verified ? "BCC ok" : repeat ? "BCC bad, confirmed by repeat" : "BCC bad");
            if (verified || repeat) {
                tag = t;
                done = true;
            }
            prev = t;
            have_prev = true;
        } else {
            log_raw(buf, n);   /* unexpected data: keep it visible for diagnosis */
        }
        n = 0;
    }

    uart_driver_delete(RFID_UART);
    gpio_reset_pin(BOARD_PIN_RFID_RX);
    gpio_set_level(BOARD_PIN_RFID_EN, 0);
    awake(false);

    if (done) {
        char s[16];
        rfid_tag_str(&tag, s);
        ESP_LOGI(TAG, "tag read: %s", s);
    } else {
        ESP_LOGI(TAG, "scan %s, no tag", s_cancel ? "cancelled" : "timed out");
    }
    s_busy = false;
    if (s_cb) {
        s_cb(done ? &tag : NULL, s_ctx);
    }
    vTaskDelete(NULL);
}

esp_err_t rfid_scan_start(int timeout_s, rfid_cb_t cb, void *ctx)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_timeout_s = timeout_s;
    s_cb = cb;
    s_ctx = ctx;
    s_cancel = false;
    s_busy = true;
    if (xTaskCreate(rfid_task, "rfid", 4096, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void rfid_scan_cancel(void)
{
    s_cancel = true;
}

bool rfid_scanning(void)
{
    return s_busy;
}

void rfid_shutdown(void)
{
    s_cancel = true;
    for (int i = 0; i < 20 && s_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    gpio_set_level(BOARD_PIN_RFID_EN, 0);
}
