/*
 * ST LSM6DSOX, plus the LIS2MDL magnetometer on its sensor hub (SDX/SCX).
 *
 * Idle: accelerometer at 26 Hz low-power with the embedded hardware pedometer, gyro off.
 *
 * Streaming (SD logging): accelerometer +-16 g and gyro +-2000 dps at 104 or 416 Hz (high
 * performance), the sensor hub reads the LIS2MDL (100 Hz continuous) at 104 Hz, and all three
 * are batched into the FIFO. INT1 (GPIO8, push-pull, active high) rises at the FIFO watermark;
 * a task then burst-reads the FIFO (tagged words, 7 bytes each) and hands the samples to the
 * callback. The pedometer keeps running (it works from any accelerometer rate >= 26 Hz).
 */
#include "sensors_priv.h"
#include "sensors.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "i2c_bus.h"

#define ADDR                 0x6B
#define WHO_AM_I_VAL         0x6C

/* Main register bank */
#define REG_FUNC_CFG_ACCESS  0x01   /* bit7: embedded functions, bit6: sensor hub bank */
#define REG_FIFO_CTRL1       0x07
#define REG_FIFO_CTRL2       0x08
#define REG_FIFO_CTRL3       0x09
#define REG_FIFO_CTRL4       0x0A
#define REG_INT1_CTRL        0x0D
#define REG_WHO_AM_I         0x0F
#define REG_CTRL1_XL         0x10
#define REG_CTRL2_G          0x11
#define REG_CTRL3_C          0x12
#define REG_CTRL6_C          0x15
#define REG_CTRL7_G          0x16
#define REG_STATUS_MASTER    0x39   /* STATUS_MASTER_MAINPAGE: bit7 WR_ONCE_DONE, bit0 SENS_HUB_ENDOP */
#define REG_FIFO_STATUS1     0x3A
#define REG_FIFO_STATUS2     0x3B
#define REG_INTERNAL_FREQ    0x63   /* INTERNAL_FREQ_FINE: ODR deviation, 0.15 %/LSB */
#define REG_FIFO_DATA_TAG    0x78

/* Embedded-functions bank */
#define REG_EMB_FUNC_EN_A    0x04   /* bit3: PEDO_EN */
#define REG_EMB_FUNC_EN_B    0x05
#define REG_STEP_COUNTER_L   0x62
#define REG_EMB_FUNC_SRC     0x64   /* bit7: PEDO_RST_STEP */

/* Sensor-hub bank */
#define REG_SENSOR_HUB_1     0x02
#define REG_MASTER_CONFIG    0x14
#define REG_SLV0_ADD         0x15
#define REG_SLV0_SUBADD      0x16
#define REG_SLV0_CONFIG      0x17
#define REG_DATAWRITE_SLV0   0x21
#define MASTER_ON            0x04
#define MASTER_PULL_UP       0x08
#define MASTER_WRITE_ONCE    0x40
#define SLV0_BATCH           0x08   /* BATCH_EXT_SENS_0_EN; SHUB_ODR[7:6] = 00 -> 104 Hz */

/* LIS2MDL */
#define MAG_ADDR             0x1E
#define MAG_WHO_AM_I         0x4F
#define MAG_WHO_AM_I_VAL     0x40
#define MAG_CFG_A            0x60
#define MAG_CFG_C            0x62
#define MAG_OUTX_L           0x68
#define MAG_CFG_A_100HZ      0x8C   /* temperature compensation, 100 Hz, continuous */
#define MAG_CFG_A_IDLE       0x83
#define MAG_CFG_C_BDU        0x10

#define CTRL1_XL_26HZ_2G     0x20
#define CTRL3_C_BDU_IFINC    0x44
#define CTRL3_C_SW_RESET     0x01
#define CTRL6_C_XL_LP        0x10   /* XL_HM_MODE = 1: low-power / normal mode */
#define FS_XL_16G            0x04
#define FS_G_2000DPS         0x0C
#define ODR_CODE_104         0x4
#define ODR_CODE_416         0x6

#define TAG_GYRO             0x01
#define TAG_ACCEL            0x02
#define TAG_SHUB0            0x0E

#define FIFO_WTM             128    /* words: ~0.14 s at 416 Hz, ~0.4 s at 104 Hz */
#define WORDS_PER_READ       32     /* 224 bytes per I2C burst */
#define CB_CAP               192    /* words collected before handing a batch over */
#define POLL_FALLBACK_MS     250

static const char *TAG = "lsm6dsox";
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;    /* bank switching vs. FIFO reads */
static uint16_t s_last_raw;
static uint32_t s_total;

static TaskHandle_t s_task;
static volatile bool s_streaming, s_stop;
static uint16_t s_rate;
static float s_rate_actual;
static bool s_mag_ok;
static imu_stream_cb_t s_cb;
static void *s_cb_ctx;
static bool s_isr_added;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t wr(uint8_t reg, uint8_t val) { return i2c_reg_write8(s_dev, reg, val); }
static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n) { return i2c_reg_read(s_dev, reg, buf, n); }

static esp_err_t emb_bank(bool on) { return wr(REG_FUNC_CFG_ACCESS, on ? 0x80 : 0x00); }
static esp_err_t shub_bank(bool on) { return wr(REG_FUNC_CFG_ACCESS, on ? 0x40 : 0x00); }

esp_err_t lsm6dsox_init(void)
{
    if (!i2c_bus_probe(ADDR)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(i2c_bus_add(ADDR, &s_dev), TAG, "add");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(rd(REG_WHO_AM_I, &who, 1), TAG, "who_am_i");
    ESP_RETURN_ON_FALSE(who == WHO_AM_I_VAL, ESP_ERR_INVALID_RESPONSE, TAG, "WHO_AM_I 0x%02x", who);

    /* Clean slate: the SPro firmware, or our own last run (the IMU stays powered while on
     * USB), may have left it configured - possibly with the sensor hub's I2C master running,
     * which a software reset does not stop. Stop the hub the ST way (accelerometer off first),
     * then reset. The reset write itself may be NACKed while the chip reboots, so check that
     * it answers again afterwards instead. */
    ESP_RETURN_ON_ERROR(emb_bank(false), TAG, "bank");
    wr(REG_CTRL1_XL, 0x00);
    shub_bank(true);
    wr(REG_MASTER_CONFIG, 0x00);
    wr(REG_SLV0_CONFIG, 0x00);
    shub_bank(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t rst = ESP_FAIL;
    for (int i = 0; i < 3 && rst != ESP_OK; i++) {
        wr(REG_CTRL3_C, CTRL3_C_SW_RESET);
        vTaskDelay(pdMS_TO_TICKS(20));   /* >= 10 ms with a 10 ms tick; reset needs ~50 us */
        who = 0;
        rst = rd(REG_WHO_AM_I, &who, 1);
        if (rst == ESP_OK && who != WHO_AM_I_VAL) {
            rst = ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_RETURN_ON_ERROR(rst, TAG, "reset");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL3_C, CTRL3_C_BDU_IFINC), TAG, "ctrl3");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL2_G, 0x00), TAG, "gyro off");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL6_C, CTRL6_C_XL_LP), TAG, "ctrl6");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL1_XL, CTRL1_XL_26HZ_2G), TAG, "ctrl1");

    /* Pedometer on, everything else in the embedded block off; reset the count. */
    ESP_RETURN_ON_ERROR(emb_bank(true), TAG, "bank");
    esp_err_t err = wr(REG_EMB_FUNC_EN_A, 0x08);
    if (err == ESP_OK) {
        err = wr(REG_EMB_FUNC_EN_B, 0x00);
    }
    if (err == ESP_OK) {
        err = wr(REG_EMB_FUNC_SRC, 0x80);
    }
    emb_bank(false);
    ESP_RETURN_ON_ERROR(err, TAG, "pedometer");

    /* The step-counter reset only lands on a later embedded-function cycle (26 Hz),
     * so wait for it and take whatever the counter reads then as the baseline. */
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t b[2] = { 0 };
    if (emb_bank(true) == ESP_OK) {
        rd(REG_STEP_COUNTER_L, b, 2);
        emb_bank(false);
    }
    s_last_raw = (uint16_t)(b[1] << 8 | b[0]);
    s_total = 0;
    ESP_LOGI(TAG, "found, accel 26 Hz low-power, pedometer on");
    return ESP_OK;
}

esp_err_t lsm6dsox_read_steps(uint32_t *steps)
{
    uint8_t b[2];
    lock();
    esp_err_t err = emb_bank(true);
    if (err == ESP_OK) {
        err = rd(REG_STEP_COUNTER_L, b, 2);
        emb_bank(false);
    }
    unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "steps");

    uint16_t raw = (uint16_t)(b[1] << 8 | b[0]);
    if (raw >= s_last_raw) {
        s_total += raw - s_last_raw;
    } else if (s_last_raw > 60000) {
        s_total += (uint32_t)raw + 65536u - s_last_raw;   /* genuine 16-bit wrap */
    } else {
        s_total += raw;                                   /* counter was reset under us */
    }
    s_last_raw = raw;
    *steps = s_total;
    return ESP_OK;
}

/* ------------------------------------------------------------ sensor hub */

/* Wait for a sensor-hub status bit (the hub cycles with the accelerometer). */
static bool shub_wait(uint8_t mask)
{
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t st = 0;
        if (rd(REG_STATUS_MASTER, &st, 1) == ESP_OK && (st & mask)) {
            return true;
        }
    }
    return false;
}

/*
 * The accelerometer is the sensor hub's trigger, and ST's sequence (AN5272) only switches the
 * I2C master on or off while the accelerometer is powered down: toggling it with the
 * accelerometer running can leave the hub stuck (STATUS_MASTER stays 0, no more reads).
 * So every one-shot operation is: XL off -> set up + MASTER_ON -> XL on -> wait -> XL off ->
 * MASTER off.
 */
static void xl_off(void) { wr(REG_CTRL1_XL, FS_XL_16G); }
static void xl_trigger(void) { wr(REG_CTRL1_XL, (ODR_CODE_104 << 4) | FS_XL_16G); }

static bool shub_once(uint8_t master_cfg, uint8_t done_mask)
{
    shub_bank(true);
    wr(REG_MASTER_CONFIG, master_cfg);
    shub_bank(false);
    uint8_t st;
    rd(REG_STATUS_MASTER, &st, 1);   /* clear a stale flag */
    xl_trigger();
    bool ok = shub_wait(done_mask);
    xl_off();
    shub_bank(true);
    wr(REG_MASTER_CONFIG, MASTER_PULL_UP);
    shub_bank(false);
    return ok;
}

static esp_err_t mag_write(uint8_t reg, uint8_t val)
{
    xl_off();
    shub_bank(true);
    wr(REG_SLV0_ADD, MAG_ADDR << 1);
    wr(REG_SLV0_SUBADD, reg);
    wr(REG_DATAWRITE_SLV0, val);
    wr(REG_SLV0_CONFIG, 0x00);
    shub_bank(false);
    return shub_once(MASTER_WRITE_ONCE | MASTER_PULL_UP | MASTER_ON, 0x80) ? ESP_OK : ESP_ERR_TIMEOUT;   /* WR_ONCE_DONE */
}

static esp_err_t mag_read(uint8_t reg, uint8_t *out, uint8_t n)
{
    xl_off();
    shub_bank(true);
    wr(REG_SLV0_ADD, (MAG_ADDR << 1) | 1);
    wr(REG_SLV0_SUBADD, reg);
    wr(REG_SLV0_CONFIG, n);
    shub_bank(false);
    if (!shub_once(MASTER_PULL_UP | MASTER_ON, 0x01)) {   /* SENS_HUB_ENDOP */
        return ESP_ERR_TIMEOUT;
    }
    shub_bank(true);
    esp_err_t err = rd(REG_SENSOR_HUB_1, out, n);
    shub_bank(false);
    return err;
}

/* One way of running the hub continuously, tried at start-up (see mag_start). */
/*
 * Measured on the SPRO (2026-11-09): the first continuous start after the LIS2MDL writes does
 * not run (STATUS_MASTER stays 0), every later one does, with or without write-once, batching or
 * the gyro. So the start is verified (SENS_HUB_ENDOP) and retried; write-once is kept on, as in
 * ST's driver.
 */
typedef struct {
    const char *name;
    bool stream_rate;       /* accelerometer at the stream rate (else 104 Hz) */
    bool gyro;
    uint8_t master_cfg;
    uint8_t slv0_cfg;
} shub_mode_t;

static const shub_mode_t s_modes[] = {
    { "stream + write-once", true, true, MASTER_WRITE_ONCE | MASTER_PULL_UP | MASTER_ON, SLV0_BATCH | 6 },
};
#define SHUB_START_TRIES 3

/* Start continuous reads in one mode; true if the hub completes cycles (SENS_HUB_ENDOP). Leaves
 * it running (accelerometer + gyro as the mode says). */
static bool shub_try(const shub_mode_t *m, uint8_t code)
{
    xl_off();
    wr(REG_CTRL2_G, 0x00);
    shub_bank(true);
    wr(REG_MASTER_CONFIG, MASTER_PULL_UP);
    wr(REG_SLV0_ADD, (MAG_ADDR << 1) | 1);
    wr(REG_SLV0_SUBADD, MAG_OUTX_L);
    wr(REG_SLV0_CONFIG, m->slv0_cfg);
    wr(REG_MASTER_CONFIG, m->master_cfg);
    shub_bank(false);
    uint8_t st = 0, out[6] = { 0 };
    rd(REG_STATUS_MASTER, &st, 1);   /* clear */
    uint8_t c = m->stream_rate ? code : ODR_CODE_104;
    wr(REG_CTRL1_XL, (uint8_t)(c << 4) | FS_XL_16G);
    if (m->gyro) {
        wr(REG_CTRL2_G, (uint8_t)(c << 4) | FS_G_2000DPS);
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    rd(REG_STATUS_MASTER, &st, 1);
    shub_bank(true);
    rd(REG_SENSOR_HUB_1, out, 6);
    shub_bank(false);
    bool ok = st & 0x01;
    ESP_LOGD(TAG, "shub mode \"%s\": %s (STATUS_MASTER 0x%02x, data %d %d %d)", m->name, ok ? "running" : "NOT running",
             st, (int16_t)(out[1] << 8 | out[0]), (int16_t)(out[3] << 8 | out[2]), (int16_t)(out[5] << 8 | out[4]));
    return ok;
}

static void shub_halt(void)
{
    xl_off();
    wr(REG_CTRL2_G, 0x00);
    shub_bank(true);
    wr(REG_MASTER_CONFIG, MASTER_PULL_UP);
    shub_bank(false);
}

/*
 * LIS2MDL on: check it answers, configure 100 Hz continuous, then start the hub reading its six
 * output bytes every 104 Hz cycle into the FIFO (verified, retried). Returns true with the
 * accelerometer and gyro running at the stream rate; false with them off.
 */
static bool mag_start(uint8_t code)
{
    uint8_t who = 0;
    if (mag_read(MAG_WHO_AM_I, &who, 1) != ESP_OK || who != MAG_WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "LIS2MDL not found on the sensor hub (WHO_AM_I 0x%02x)", who);
        return false;
    }
    if (mag_write(MAG_CFG_A, MAG_CFG_A_100HZ) != ESP_OK || mag_write(MAG_CFG_C, MAG_CFG_C_BDU) != ESP_OK) {
        ESP_LOGW(TAG, "LIS2MDL configuration failed");
        return false;
    }
    for (int t = 1; t <= SHUB_START_TRIES; t++) {
        if (shub_try(&s_modes[0], code)) {
            ESP_LOGI(TAG, "sensor hub running (attempt %d)", t);
            return true;                 /* left running */
        }
        shub_halt();
    }
    ESP_LOGW(TAG, "sensor hub did not start: magnetometer not recorded");
    return false;
}

/* Diagnostics: sensor-hub state (call with the lock held). */
static void shub_dump(const char *when)
{
    uint8_t st = 0, out[6] = { 0 }, cfg[3] = { 0 }, mc = 0, f3 = 0, f4 = 0;
    rd(REG_STATUS_MASTER, &st, 1);
    rd(REG_FIFO_CTRL3, &f3, 1);
    rd(REG_FIFO_CTRL4, &f4, 1);
    shub_bank(true);
    rd(REG_SENSOR_HUB_1, out, 6);
    rd(REG_MASTER_CONFIG, &mc, 1);
    rd(REG_SLV0_ADD, cfg, 3);
    shub_bank(false);
    ESP_LOGI(TAG, "shub %s: STATUS_MASTER 0x%02x MASTER_CONFIG 0x%02x SLV0 add 0x%02x sub 0x%02x cfg 0x%02x "
             "FIFO_CTRL3 0x%02x CTRL4 0x%02x data %d %d %d", when, st, mc, cfg[0], cfg[1], cfg[2], f3, f4,
             (int16_t)(out[1] << 8 | out[0]), (int16_t)(out[3] << 8 | out[2]), (int16_t)(out[5] << 8 | out[4]));
}

static void mag_stop(void)
{
    xl_off();
    shub_bank(true);
    wr(REG_MASTER_CONFIG, MASTER_PULL_UP);
    wr(REG_SLV0_CONFIG, 0x00);
    shub_bank(false);
    mag_write(MAG_CFG_A, MAG_CFG_A_IDLE);   /* LIS2MDL idle (~2 uA) */
}

/* -------------------------------------------------------------- streaming */

static void IRAM_ATTR int1_isr(void *arg)
{
    gpio_intr_disable(BOARD_PIN_IMU_INT1);   /* level interrupt: re-enabled after the FIFO read */
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void int1_setup(bool on)
{
    if (!on) {
        gpio_intr_disable(BOARD_PIN_IMU_INT1);
        gpio_wakeup_disable(BOARD_PIN_IMU_INT1);
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = BIT64(BOARD_PIN_IMU_INT1),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_HIGH_LEVEL,
    };
    gpio_config(&io);
    gpio_sleep_sel_dis(BOARD_PIN_IMU_INT1);
    if (!s_isr_added && gpio_isr_handler_add(BOARD_PIN_IMU_INT1, int1_isr, NULL) == ESP_OK) {
        s_isr_added = true;
    }
    gpio_wakeup_enable(BOARD_PIN_IMU_INT1, GPIO_INTR_HIGH_LEVEL);
    gpio_intr_enable(BOARD_PIN_IMU_INT1);
}

static esp_err_t stream_config(void)
{
    uint8_t code = s_rate == 416 ? ODR_CODE_416 : ODR_CODE_104;
    lock();
    esp_err_t err = wr(REG_FIFO_CTRL4, 0x00);                         /* bypass: empties the FIFO */
    if (err == ESP_OK) err = wr(REG_CTRL6_C, 0x00);                   /* accel high performance */
    if (err == ESP_OK) err = wr(REG_CTRL7_G, 0x00);                   /* gyro high performance */
    s_mag_ok = err == ESP_OK && mag_start(code);   /* true: accel + gyro + hub already running */
    if (err == ESP_OK && !s_mag_ok) {
        err = wr(REG_CTRL1_XL, (uint8_t)(code << 4) | FS_XL_16G);
        if (err == ESP_OK) err = wr(REG_CTRL2_G, (uint8_t)(code << 4) | FS_G_2000DPS);
    }
    unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "odr");
    vTaskDelay(pdMS_TO_TICKS(30));

    lock();
    int8_t fine = 0;
    rd(REG_INTERNAL_FREQ, (uint8_t *)&fine, 1);
    s_rate_actual = s_rate * (1.0f + 0.0015f * fine);
    err = wr(REG_FIFO_CTRL1, FIFO_WTM & 0xFF);
    if (err == ESP_OK) err = wr(REG_FIFO_CTRL2, (FIFO_WTM >> 8) & 0x01);
    if (err == ESP_OK) err = wr(REG_FIFO_CTRL3, (uint8_t)(code << 4) | code);   /* batch gyro + accel */
    if (err == ESP_OK) err = wr(REG_INT1_CTRL, 0x08);                 /* INT1_FIFO_TH */
    if (err == ESP_OK) err = wr(REG_FIFO_CTRL4, 0x06);                /* continuous mode */
    unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "fifo");
    ESP_LOGI(TAG, "streaming accel +-16 g + gyro +-2000 dps at %u Hz (actual %.2f Hz, trim %d), mag %s",
             s_rate, s_rate_actual, fine, s_mag_ok ? "104 Hz" : "OFF");
    return ESP_OK;
}

static void stream_unconfig(void)
{
    lock();
    wr(REG_INT1_CTRL, 0x00);
    wr(REG_FIFO_CTRL4, 0x00);
    wr(REG_FIFO_CTRL3, 0x00);
    if (s_mag_ok) {
        mag_stop();
    }
    wr(REG_CTRL2_G, 0x00);
    wr(REG_CTRL6_C, CTRL6_C_XL_LP);
    wr(REG_CTRL1_XL, CTRL1_XL_26HZ_2G);   /* back to the pedometer-only state */
    unlock();
}

static int16_t s_acc[CB_CAP][3], s_gyr[CB_CAP][3], s_mag[CB_CAP][3];

static void imu_task(void *arg)
{
    uint8_t buf[WORDS_PER_READ * 7];
    bool first = true;
    uint32_t stats_acc = 0, stats_gyr = 0, stats_mag = 0, stats_reads = 0, stats_ovr = 0, stats_unknown = 0;
    int64_t stats_t0 = esp_timer_get_time();

    int1_setup(true);
    while (!s_stop) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_FALLBACK_MS));
        if (s_stop) {
            break;
        }
        uint8_t st[2] = { 0 };
        lock();
        esp_err_t err = rd(REG_FIFO_STATUS1, st, 2);
        unlock();
        uint32_t level = err == ESP_OK ? ((uint32_t)(st[1] & 0x03) << 8 | st[0]) : 0;
        if (st[1] & 0x40) {
            stats_ovr++;
        }
        int64_t t_read = esp_timer_get_time();
        imu_batch_t b = { .acc = s_acc, .gyr = s_gyr, .mag = s_mag };
        while (level) {
            uint32_t n = level < WORDS_PER_READ ? level : WORDS_PER_READ;
            lock();
            err = rd(REG_FIFO_DATA_TAG, buf, n * 7);
            unlock();
            if (err != ESP_OK) {
                break;
            }
            stats_reads++;
            level -= n;
            for (uint32_t w = 0; w < n; w++) {
                const uint8_t *p = &buf[w * 7];
                int16_t xyz[3] = { (int16_t)(p[2] << 8 | p[1]), (int16_t)(p[4] << 8 | p[3]),
                                   (int16_t)(p[6] << 8 | p[5]) };
                switch (p[0] >> 3) {
                case TAG_ACCEL: memcpy(s_acc[b.n_acc++], xyz, sizeof(xyz)); break;
                case TAG_GYRO:  memcpy(s_gyr[b.n_gyr++], xyz, sizeof(xyz)); break;
                case TAG_SHUB0: memcpy(s_mag[b.n_mag++], xyz, sizeof(xyz)); break;
                default:        stats_unknown++; break;
                }
            }
            bool full = b.n_acc > CB_CAP - WORDS_PER_READ || b.n_gyr > CB_CAP - WORDS_PER_READ ||
                        b.n_mag > CB_CAP - WORDS_PER_READ;
            if (full || level == 0) {
                if (first) {
                    /* the newest accel sample was taken about now: date the first one back */
                    b.first_us = t_read - (int64_t)((b.n_acc ? b.n_acc - 1 : 0) * 1e6f / s_rate_actual);
                    first = false;
                }
                stats_acc += b.n_acc;
                stats_gyr += b.n_gyr;
                stats_mag += b.n_mag;
                if (s_cb && (b.n_acc || b.n_gyr || b.n_mag)) {
                    s_cb(&b, s_cb_ctx);
                }
                b = (imu_batch_t){ .acc = s_acc, .gyr = s_gyr, .mag = s_mag };
            }
        }
        gpio_intr_enable(BOARD_PIN_IMU_INT1);

        if (t_read - stats_t0 >= 60000000) {
            if (s_mag_ok && stats_mag == 0) {
                lock();
                shub_dump("no mag samples");   /* the hub stopped: show its state */
                unlock();
            }
            ESP_LOGI(TAG, "stream: acc %lu gyr %lu mag %lu in %lld s, %lu reads, %lu overruns, %lu other tags",
                     (unsigned long)stats_acc, (unsigned long)stats_gyr, (unsigned long)stats_mag,
                     (t_read - stats_t0) / 1000000, (unsigned long)stats_reads, (unsigned long)stats_ovr,
                     (unsigned long)stats_unknown);
            stats_acc = stats_gyr = stats_mag = stats_reads = stats_ovr = stats_unknown = 0;
            stats_t0 = t_read;
        }
    }
    int1_setup(false);
    stream_unconfig();
    s_streaming = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t lsm6dsox_stream_start(uint16_t rate_hz, imu_stream_cb_t cb, void *ctx, float *actual_hz)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((rate_hz != 104 && rate_hz != 416) || !cb || s_streaming) {
        return ESP_ERR_INVALID_ARG;
    }
    s_rate = rate_hz;
    s_cb = cb;
    s_cb_ctx = ctx;
    ESP_RETURN_ON_ERROR(stream_config(), TAG, "config");
    if (actual_hz) {
        *actual_hz = s_rate_actual;
    }
    s_stop = false;
    s_streaming = true;
    if (xTaskCreate(imu_task, "imu", 4096, NULL, 5, &s_task) != pdPASS) {
        s_streaming = false;
        stream_unconfig();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool lsm6dsox_mag_ok(void)
{
    return s_mag_ok;
}

void lsm6dsox_stream_stop(void)
{
    if (!s_streaming || !s_task) {
        return;
    }
    s_stop = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 40 && s_streaming; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void lsm6dsox_power_down(void)
{
    if (!s_dev) {
        return;
    }
    lsm6dsox_stream_stop();
    lock();
    if (emb_bank(true) == ESP_OK) {
        wr(REG_EMB_FUNC_EN_A, 0x00);
        emb_bank(false);
    }
    wr(REG_CTRL1_XL, 0x00);
    wr(REG_CTRL2_G, 0x00);
    unlock();
}
