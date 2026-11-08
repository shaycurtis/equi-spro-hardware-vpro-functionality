#include "max30001.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "board.h"

/* Register addresses */
#define REG_STATUS      0x01
#define REG_EN_INT      0x02
#define REG_EN_INT2     0x03
#define REG_MNGR_INT    0x04
#define REG_SW_RST      0x08
#define REG_SYNCH       0x09
#define REG_FIFO_RST    0x0A
#define REG_INFO        0x0F
#define REG_CNFG_GEN    0x10
#define REG_CNFG_CAL    0x12
#define REG_CNFG_EMUX   0x14
#define REG_CNFG_ECG    0x15
#define REG_ECG_FIFO    0x21

/* CNFG_GEN: EN_ECG (bit19), FMSTR=00 (32768 Hz -> 512 sps base), lead-off and bias off */
#define GEN_EN_ECG      (1u << 19)

/* CNFG_ECG: RATE[23:22]=00 (512 sps), GAIN[17:16]=10 (80 V/V), DHPF[14]=1 (0.5 Hz), DLPF[13:12]=01 (40 Hz) */
#define ECG_CFG         ((0u << 22) | (2u << 16) | (1u << 14) | (1u << 12))

/* CNFG_CAL: EN_VCAL[22], VMODE[21]=bipolar, VMAG[20]=0.5 mV, FCAL[14:12]=100 (FMSTR/2^15 = 1 Hz), FIFTY[11] */
#define CAL_TEST        ((1u << 22) | (1u << 21) | (1u << 20) | (4u << 12) | (1u << 11))

/* CNFG_EMUX: OPENP[21], OPENN[20] (isolate electrodes), CALP_SEL[19:18]=10 VCALP, CALN_SEL[17:16]=11 VCALN */
#define EMUX_TEST       ((1u << 21) | (1u << 20) | (2u << 18) | (3u << 16))
#define EMUX_NORMAL     0u

/* ECG_FIFO word: data[23:6] (18-bit two's complement), ETAG[5:3] */
#define ETAG_VALID      0
#define ETAG_FAST       1
#define ETAG_VALID_EOF  2
#define ETAG_FAST_EOF   3
#define ETAG_EMPTY      6
#define ETAG_OVERFLOW   7

#define SPI_HZ          4000000   /* max 12 MHz; kept low because SDO only swings to OVDD = 1.8 V */

static const char *TAG = "max30001";
static spi_device_handle_t s_spi;

static esp_err_t xfer(uint8_t reg, bool read, uint32_t wdata, uint32_t *rdata)
{
    spi_transaction_t t = {
        .length = 32,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = { (uint8_t)((reg << 1) | (read ? 1 : 0)),
                     (uint8_t)(wdata >> 16), (uint8_t)(wdata >> 8), (uint8_t)wdata },
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK && rdata) {
        *rdata = ((uint32_t)t.rx_data[1] << 16) | ((uint32_t)t.rx_data[2] << 8) | t.rx_data[3];
    }
    return err;
}

static esp_err_t wr(uint8_t reg, uint32_t v) { return xfer(reg, false, v, NULL); }
static esp_err_t rd(uint8_t reg, uint32_t *v) { return xfer(reg, true, 0, v); }

/* Datasheet (INFO): the first command after power-up or SW_RST does not read back valid
 * data, so follow every reset with a throw-away read. */
static esp_err_t soft_reset(void)
{
    uint32_t dummy;
    esp_err_t err = wr(REG_SW_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    rd(REG_INFO, &dummy);
    return err;
}

esp_err_t max30001_bus_init(void)
{
    if (s_spi) {
        return ESP_OK;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_PIN_ECG_MOSI,
        .miso_io_num = BOARD_PIN_ECG_MISO,
        .sclk_io_num = BOARD_PIN_ECG_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED), TAG, "bus");
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = BOARD_PIN_ECG_CS,
        .queue_size = 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "device");
    return ESP_OK;
}

esp_err_t max30001_probe(uint32_t *info)
{
    ESP_RETURN_ON_ERROR(soft_reset(), TAG, "reset");
    uint32_t v = 0;
    ESP_RETURN_ON_ERROR(rd(REG_INFO, &v), TAG, "info");
    *info = v;
    /* INFO[23:20] is a fixed 0101 pattern (datasheet Table 22) for interface verification. */
    if ((v & 0xF00000) != 0x500000) {
        ESP_LOGE(TAG, "INFO = 0x%06lx (expected 0x5xxxxx) - SPI or power problem", (unsigned long)v);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "found, INFO 0x%06lx (revision %lu)", (unsigned long)v, (unsigned long)((v >> 16) & 0xF));
    return ESP_OK;
}

esp_err_t max30001_ecg_start(bool test_signal)
{
    ESP_RETURN_ON_ERROR(soft_reset(), TAG, "reset");
    ESP_RETURN_ON_ERROR(wr(REG_EN_INT, 0), TAG, "en_int");      /* polled, no interrupts */
    ESP_RETURN_ON_ERROR(wr(REG_EN_INT2, 0), TAG, "en_int2");
    ESP_RETURN_ON_ERROR(wr(REG_CNFG_GEN, GEN_EN_ECG), TAG, "gen");
    ESP_RETURN_ON_ERROR(wr(REG_CNFG_CAL, test_signal ? CAL_TEST : 0), TAG, "cal");
    ESP_RETURN_ON_ERROR(wr(REG_CNFG_EMUX, test_signal ? EMUX_TEST : EMUX_NORMAL), TAG, "emux");
    ESP_RETURN_ON_ERROR(wr(REG_CNFG_ECG, ECG_CFG), TAG, "ecg");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(wr(REG_SYNCH, 0), TAG, "synch");         /* start conversions */
    ESP_RETURN_ON_ERROR(wr(REG_FIFO_RST, 0), TAG, "fifo_rst");
    ESP_LOGI(TAG, "ECG started: 512 sps, gain 80, %s", test_signal ? "INTERNAL 1 Hz TEST SIGNAL" : "electrodes");
    return ESP_OK;
}

size_t max30001_ecg_read(int32_t *out, size_t max, bool *overflow)
{
    size_t n = 0;
    *overflow = false;
    while (n < max) {
        uint32_t w;
        if (rd(REG_ECG_FIFO, &w) != ESP_OK) {
            break;
        }
        uint32_t etag = (w >> 3) & 0x7;
        if (etag == ETAG_EMPTY) {
            break;
        }
        if (etag == ETAG_OVERFLOW) {
            *overflow = true;
            wr(REG_FIFO_RST, 0);
            break;
        }
        int32_t s = (int32_t)(w >> 6) & 0x3FFFF;
        if (s & 0x20000) {
            s -= 0x40000;   /* sign-extend 18 bits */
        }
        out[n++] = s;
        if (etag == ETAG_VALID_EOF || etag == ETAG_FAST_EOF) {
            break;
        }
    }
    return n;
}

void max30001_stop(void)
{
    if (s_spi) {
        wr(REG_CNFG_GEN, 0);
        wr(REG_SW_RST, 0);
    }
}
