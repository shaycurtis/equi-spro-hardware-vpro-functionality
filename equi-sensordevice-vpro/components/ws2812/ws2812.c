#include "ws2812.h"

#include <stdlib.h>
#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

/*
 * RMT at 10 MHz -> 1 tick = 100 ns.
 * WS2812B-2020 timing (datasheet): T0H 220-380 ns, T0L 580-1000 ns,
 *                                  T1H 580-1000 ns, T1L 580-1000 ns, reset > 280 us.
 */
#define RMT_RES_HZ   (10 * 1000 * 1000)
#define T0H_TICKS    3   /* 300 ns */
#define T0L_TICKS    9   /* 900 ns */
#define T1H_TICKS    8   /* 800 ns */
#define T1L_TICKS    6   /* 600 ns */
#define RESET_US     300

static const char *TAG = "ws2812";

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint8_t *s_buf;      /* GRB, 3 bytes per LED */
static uint32_t s_count;

esp_err_t ws2812_init(gpio_num_t gpio, uint32_t led_count)
{
    s_count = led_count;
    s_buf = calloc(led_count, 3);
    ESP_RETURN_ON_FALSE(s_buf, ESP_ERR_NO_MEM, TAG, "no mem");

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &s_chan), TAG, "rmt channel");

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H_TICKS, .level1 = 0, .duration1 = T0L_TICKS },
        .bit1 = { .level0 = 1, .duration0 = T1H_TICKS, .level1 = 0, .duration1 = T1L_TICKS },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_enc), TAG, "encoder");
    /* The channel is enabled only while transmitting: an enabled RMT channel holds a
     * power-management lock that would stop the chip from entering light sleep. */
    return ESP_OK;
}

void ws2812_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= s_count) {
        return;
    }
    s_buf[index * 3 + 0] = g;
    s_buf[index * 3 + 1] = r;
    s_buf[index * 3 + 2] = b;
}

void ws2812_clear(void)
{
    memset(s_buf, 0, s_count * 3);
}

esp_err_t ws2812_show(void)
{
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "rmt enable");
    esp_err_t err = rmt_transmit(s_chan, s_enc, s_buf, s_count * 3, &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_chan, 100);
    }
    esp_rom_delay_us(RESET_US); /* line idles low = latch/reset */
    rmt_disable(s_chan);
    ESP_RETURN_ON_ERROR(err, TAG, "tx");
    return ESP_OK;
}
