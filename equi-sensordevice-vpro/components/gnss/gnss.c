#include "gnss.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "board.h"

#define GNSS_UART      UART_NUM_1
#define LISTEN_MS      1300          /* NMEA is 1 Hz, so this always spans a burst */

static const char *TAG = "gnss";

static const int k_bauds[] = { BOARD_GPS_BAUD, 460800, 9600, 230400, 921600 };

/* Look for something shaped like an NMEA sentence: "$G" ... "*". */
static bool looks_like_nmea(const uint8_t *b, int n)
{
    for (int i = 0; i + 1 < n; i++) {
        if (b[i] == '$' && (b[i + 1] == 'G' || b[i + 1] == 'P')) {
            for (int j = i + 2; j < n && j < i + 100; j++) {
                if (b[j] == '*') {
                    return true;
                }
                if (b[j] < 0x20 || b[j] > 0x7e) {
                    break;
                }
            }
        }
    }
    return false;
}

static bool listen(int ms)
{
    static uint8_t buf[512];
    uart_flush_input(GNSS_UART);
    int n = uart_read_bytes(GNSS_UART, buf, sizeof(buf), pdMS_TO_TICKS(ms));
    return n > 0 && looks_like_nmea(buf, n);
}

static void send_pair(const char *body)
{
    uint8_t cs = 0;
    for (const char *p = body; *p; p++) {
        cs ^= (uint8_t)*p;
    }
    char msg[48];
    int len = snprintf(msg, sizeof(msg), "$%s*%02X\r\n", body, cs);
    uart_write_bytes(GNSS_UART, msg, len);
    uart_wait_tx_done(GNSS_UART, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "sent %.*s", len - 2, msg);
}

static void gnss_task(void *arg)
{
    /* Verified pin order first; the swap is kept as a fallback for other board revisions. */
    const gpio_num_t pins[2] = { BOARD_PIN_GPS_RX, BOARD_PIN_GPS_TX };
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(GNSS_UART, 1024, 0, 0, NULL, 0);
    uart_param_config(GNSS_UART, &cfg);

    int rx = -1, tx = -1, baud = 0;
    /* Listen only (no TX pin yet) so we never drive the module's TXD line. */
    for (int p = 0; p < 2 && rx < 0; p++) {
        uart_set_pin(GNSS_UART, UART_PIN_NO_CHANGE, pins[p], UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        for (size_t b = 0; b < sizeof(k_bauds) / sizeof(k_bauds[0]); b++) {
            uart_set_baudrate(GNSS_UART, k_bauds[b]);
            if (listen(LISTEN_MS)) {
                rx = pins[p];
                tx = pins[1 - p];
                baud = k_bauds[b];
                break;
            }
        }
    }

    if (rx < 0) {
        ESP_LOGW(TAG, "no NMEA output found (already in backup mode, or not responding)");
    } else {
        ESP_LOGI(TAG, "NMEA on GPIO%d at %d baud -> TX on GPIO%d", rx, baud, tx);
        uart_set_pin(GNSS_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        bool stopped = false;
        for (int attempt = 0; attempt < 3 && !stopped; attempt++) {
            send_pair("PAIR650,0");          /* enter backup mode, no timed wake-up */
            vTaskDelay(pdMS_TO_TICKS(300));
            stopped = !listen(LISTEN_MS);
        }
        if (stopped) {
            ESP_LOGI(TAG, "GNSS in backup mode (NMEA stopped)");
        } else {
            ESP_LOGW(TAG, "GNSS still outputting NMEA after PAIR650");
        }
    }

    uart_driver_delete(GNSS_UART);
    gpio_reset_pin(BOARD_PIN_GPS_TX);
    gpio_reset_pin(BOARD_PIN_GPS_RX);
    vTaskDelete(NULL);
}

void gnss_standby_async(void)
{
    xTaskCreate(gnss_task, "gnss", 3072, NULL, 2, NULL);
}
