#include "power.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "board.h"
#include "status_led.h"

static const char *TAG = "power";

#define LATCH_MAGIC          0x50574C41u  /* "PWLA" */
#define MAX_SHUTDOWN_CBS     8
#define DEBOUNCE_MS          50
#define TASK_PERIOD_MS       250
#define FAULT_WINDOW_MS      3000
#define USB_STOP_DEBOUNCE_MS 2000   /* USB must stay present this long before auto-stop */

/* Survives software/panic/watchdog resets (not power-on): was the latch on? */
static RTC_NOINIT_ATTR uint32_t s_latched_magic;

static power_shutdown_cb_t s_cbs[MAX_SHUTDOWN_CBS];
static int s_ncbs;
static TaskHandle_t s_task;
static bool s_resume;
static volatile power_charge_t s_charge;
static volatile bool s_charge_led = true;   /* false while the app uses D2 (e.g. RFID prompt) */

/* ---------------------------------------------------------------- helpers */

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static inline bool btn_raw(void) { return gpio_get_level(BOARD_PIN_PWR_BTN); }
bool power_usb_present(void) { return gpio_get_level(BOARD_PIN_PWR_GD); }
power_charge_t power_charge_state(void) { return s_charge; }

/* Charger STAT tracker: steady low = charging, steady high = done, toggling = fault. */
typedef struct {
    int last;
    int64_t edges[2];
    int nedges;
} chg_trk_t;

static power_charge_t chg_update(chg_trk_t *t, int64_t t_ms)
{
    if (!power_usb_present()) {
        t->last = -1;
        t->nedges = 0;
        return POWER_CHARGE_NO_USB;
    }
    int level = gpio_get_level(BOARD_PIN_CHG_IND);
    if (t->last >= 0 && level != t->last) {
        t->edges[0] = t->edges[1];
        t->edges[1] = t_ms;
        if (t->nedges < 2) {
            t->nedges++;
        }
    }
    t->last = level;
    if (t->nedges == 2 && t_ms - t->edges[0] < FAULT_WINDOW_MS) {
        return POWER_CHARGE_FAULT;
    }
    return level ? POWER_CHARGE_DONE : POWER_CHARGE_CHARGING;
}

static led_colour_t charge_colour(power_charge_t c)
{
    switch (c) {
    case POWER_CHARGE_CHARGING: return LED_RED;
    case POWER_CHARGE_DONE:     return LED_GREEN;
    case POWER_CHARGE_FAULT:    return LED_ORANGE;
    default:                    return LED_OFF;
    }
}

static const char *charge_name(power_charge_t c)
{
    static const char *n[] = { "no USB", "charging", "full", "FAULT" };
    return n[c];
}

/* Wait until the button has been released (debounced). */
static void wait_btn_release(void)
{
    int64_t released_at = -1;
    for (;;) {
        if (btn_raw()) {
            released_at = -1;
        } else if (released_at < 0) {
            released_at = now_ms();
        } else if (now_ms() - released_at >= DEBOUNCE_MS) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void latch_on(void)
{
    gpio_set_level(BOARD_PIN_PMIC_EN, 1);
    gpio_hold_en(BOARD_PIN_PMIC_EN);     /* keep latched through a crash/SW reset */
    s_latched_magic = LATCH_MAGIC;
}

static void latch_off(void)
{
    s_latched_magic = 0;
    gpio_hold_dis(BOARD_PIN_PMIC_EN);
    gpio_set_level(BOARD_PIN_PMIC_EN, 0);
}

static void rails_off(void)
{
    gpio_set_level(BOARD_PIN_VCC_CTRL, 0);
    gpio_set_level(BOARD_PIN_RFID_EN, 0);
    gpio_set_level(BOARD_PIN_MOTOR, 0);
    gpio_set_level(BOARD_PIN_BUZZER, 0);
}

/* ------------------------------------------------------ off / charge-only */

static void __attribute__((noreturn)) charge_only_sleep(void)
{
    /* Sample STAT for ~1.2 s so a 1 Hz fault blink is recognised. */
    chg_trk_t trk = { .last = -1 };
    power_charge_t c = POWER_CHARGE_NO_USB;
    for (int i = 0; i < 12; i++) {
        c = chg_update(&trk, now_ms());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /*
     * Device is "off": D1 dark, D2 shows the charge state (latched through deep sleep).
     * Order matters: after a deep-sleep wake both modes start OFF while the LEDs still
     * show the latched colour, so set D2 first - the first frame written is then the
     * final one and the LED never flickers on each 10 s wake.
     */
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_SOLID, charge_colour(c));
    status_led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_OFF);
    status_led_override_clear();
    ESP_LOGI(TAG, "Charge-only: %s, sleeping %d s (button wakes)", charge_name(c), POWER_CHARGE_POLL_S);

    wait_btn_release(); /* otherwise ext0 would wake us immediately */

    /* GPIO4 is an RTC IO: wake when the power button drives it high. */
    rtc_gpio_pullup_dis(BOARD_PIN_PWR_BTN);
    rtc_gpio_pulldown_en(BOARD_PIN_PWR_BTN);
    esp_sleep_enable_ext0_wakeup(BOARD_PIN_PWR_BTN, 1);
    esp_sleep_enable_timer_wakeup((uint64_t)POWER_CHARGE_POLL_S * 1000000ULL);

    /* PMIC_EN floats in deep sleep; D4 + R23 (100k) keep the switch enable low. */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_deep_sleep_start();
}

static void __attribute__((noreturn)) power_off_now(void)
{
    status_led_set(LED_OFF);
    latch_off();
    /* While the button is held it powers the rail itself (via D6). */
    wait_btn_release();
    ESP_LOGI(TAG, "Latch released");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Still alive: USB is holding the rail up -> charge-only mode. */
    ESP_LOGI(TAG, "Still powered (USB %s)", power_usb_present() ? "present" : "absent");
    charge_only_sleep();
}

/* ------------------------------------------------------------------ public */

void power_early_init(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    s_resume = (s_latched_magic == LATCH_MAGIC) &&
               (rr == ESP_RST_SW || rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);
    if (!s_resume) {
        s_latched_magic = 0;
    }

    /* Latch: set the level we want before releasing any hold so the pad doesn't glitch. */
    gpio_set_level(BOARD_PIN_PMIC_EN, s_resume ? 1 : 0);
    gpio_config_t latch = { .pin_bit_mask = BIT64(BOARD_PIN_PMIC_EN), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&latch);
    gpio_hold_dis(BOARD_PIN_PMIC_EN);
    if (s_resume) {
        gpio_hold_en(BOARD_PIN_PMIC_EN);
    }

    /* Switched peripherals off until their drivers want them. */
    const gpio_num_t outs[] = { BOARD_PIN_VCC_CTRL, BOARD_PIN_RFID_EN, BOARD_PIN_MOTOR, BOARD_PIN_BUZZER };
    for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
        gpio_set_level(outs[i], 0);
    }
    gpio_config_t out = {
        .pin_bit_mask = BIT64(BOARD_PIN_VCC_CTRL) | BIT64(BOARD_PIN_RFID_EN) |
                        BIT64(BOARD_PIN_MOTOR) | BIT64(BOARD_PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    /* Button back to a digital input after ext0 wake; it floats when released (D7). */
    rtc_gpio_deinit(BOARD_PIN_PWR_BTN);
    gpio_config_t btn = {
        .pin_bit_mask = BIT64(BOARD_PIN_PWR_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&btn);

    /* PWR_GD (divider) and CHG_IND (100k pull-up) have external pulls. */
    gpio_config_t in = {
        .pin_bit_mask = BIT64(BOARD_PIN_PWR_GD) | BIT64(BOARD_PIN_CHG_IND),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
}

void power_boot(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    bool usb = power_usb_present();
    bool btn = btn_raw();
    ESP_LOGI(TAG, "Boot: reset=%d wake=0x%lx usb=%d button=%d resume=%d",
             esp_reset_reason(), (unsigned long)causes, usb, btn, s_resume);

    if (s_resume) {
        ESP_LOGW(TAG, "Recovered from reset while running; staying on");
        latch_on();
        return;
    }

    if (btn) {
        status_led_set(LED_WHITE);  /* "keep holding" */
        int64_t released_at = -1;
        while (now_ms() < POWER_HOLD_ON_MS) {
            if (btn_raw()) {
                released_at = -1;
            } else if (released_at < 0) {
                released_at = now_ms();
            } else if (now_ms() - released_at >= DEBOUNCE_MS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (released_at < 0 || now_ms() - released_at < DEBOUNCE_MS) {
            latch_on();
            ESP_LOGI(TAG, "Power latched on at %lld ms", now_ms());
            status_led_flash(LED_GREEN, 2, 150, 100);
            return;
        }
        ESP_LOGI(TAG, "Button released before %d ms, not powering on", POWER_HOLD_ON_MS);
        status_led_override_clear();
    }

    if (power_usb_present()) {
        charge_only_sleep();
    }
    power_off_now();
}

void power_register_shutdown_cb(power_shutdown_cb_t cb)
{
    if (s_ncbs < MAX_SHUTDOWN_CBS) {
        s_cbs[s_ncbs++] = cb;
    }
}

void power_shutdown(void)
{
    ESP_LOGI(TAG, "Shutting down...");
    status_led_set(LED_ORANGE);
    for (int i = s_ncbs - 1; i >= 0; i--) {
        s_cbs[i]();
    }
    rails_off();
    /* Stop the "on" breathe / any app pattern; charge-only mode re-applies D2 if on USB. */
    status_led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_OFF);
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_OFF, LED_OFF);
    status_led_flash(LED_ORANGE, 3, 100, 100);
    power_off_now();
}

/* -------------------------------------------------------------- run task */

/*
 * Automatic light sleep. By default ESP-IDF switches every GPIO to an isolated (floating)
 * state during light sleep. On this board that would let PMIC_EN float and the TPS22810
 * would cut the 3.3 V rail, so pins that control power or are wake inputs keep their
 * normal configuration while asleep.
 */
static void sleep_pins_setup(void)
{
    const gpio_num_t keep[] = {
        BOARD_PIN_PMIC_EN,   /* power latch (also pad-held) */
        BOARD_PIN_VCC_CTRL,  /* MAX30001 1.8 V rails: must stay off */
        BOARD_PIN_RFID_EN,   /* RFID power: must stay off (Q3 base would float) */
        BOARD_PIN_MOTOR,
        BOARD_PIN_BUZZER,
        BOARD_PIN_LED,       /* WS2812 data stays low */
        BOARD_PIN_PWR_BTN,   /* keeps its pull-down */
        BOARD_PIN_SCAN_BTN,
    };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        gpio_sleep_sel_dis(keep[i]);
    }
    /* Buttons wake the chip from light sleep (level-triggered). */
    gpio_wakeup_enable(BOARD_PIN_PWR_BTN, GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable(BOARD_PIN_SCAN_BTN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

static void apply_charge_led(void)
{
    status_led_set_mode(LED_IDX_CHARGE, s_charge == POWER_CHARGE_NO_USB ? LED_MODE_OFF : LED_MODE_SOLID,
                        charge_colour(s_charge));
}

static void power_task(void *arg)
{
    chg_trk_t trk = { .last = -1 };
    bool armed = false;   /* ignore the press that powered us on until it's released */
    int64_t usb_since = -1;   /* when USB appeared after running on battery */

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TASK_PERIOD_MS));

        if (!btn_raw()) {
            armed = true;
        } else if (armed) {
            int64_t t0 = now_ms();
            int64_t released_at = -1;
            bool shown = false;
            while (now_ms() - t0 < POWER_HOLD_OFF_MS) {
                if (btn_raw()) {
                    released_at = -1;
                } else if (released_at < 0) {
                    released_at = now_ms();
                } else if (now_ms() - released_at >= DEBOUNCE_MS) {
                    break;
                }
                if (!shown && now_ms() - t0 > 300) {
                    status_led_set(LED_ORANGE);   /* "keep holding to power off" */
                    shown = true;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (now_ms() - t0 >= POWER_HOLD_OFF_MS) {
                power_shutdown();
            }
            ESP_LOGI(TAG, "Short press (%lld ms), ignored", now_ms() - t0);
            status_led_override_clear();
        }

        power_charge_t c = chg_update(&trk, now_ms());

        /* Plugged in while running on battery: end the session and go to charge-only.
         * Booting on USB (bench use) keeps running; only a battery -> USB transition counts. */
        if (POWER_AUTO_STOP_ON_USB && c != POWER_CHARGE_NO_USB && s_charge == POWER_CHARGE_NO_USB) {
            if (usb_since < 0) {
                usb_since = now_ms();
            } else if (now_ms() - usb_since >= USB_STOP_DEBOUNCE_MS) {
                ESP_LOGI(TAG, "USB plugged in: ending the session, charge-only");
                power_shutdown();
            }
            continue;   /* keep s_charge at "no USB" until the debounce settles */
        }
        usb_since = -1;

        if (c != s_charge) {
            ESP_LOGI(TAG, "Charge state: %s", charge_name(c));
            s_charge = c;
            if (s_charge_led) {
                apply_charge_led();
            }
        }
    }
}

void power_set_charge_led(bool enable)
{
    s_charge_led = enable;
    if (enable) {
        apply_charge_led();
    }
}

void power_start(void)
{
    /* Initial charge state so readers don't see "no USB" before the task's first poll. */
    if (power_usb_present()) {
        s_charge = gpio_get_level(BOARD_PIN_CHG_IND) ? POWER_CHARGE_DONE : POWER_CHARGE_CHARGING;
    }
    if (s_charge_led) {
        apply_charge_led();
    }
    sleep_pins_setup();
    /* The button is polled every TASK_PERIOD_MS (the level wake-up gets the chip out of light
     * sleep while it is held), so no edge interrupt is needed. */
    xTaskCreate(power_task, "power", 3072, NULL, 10, &s_task);
}
