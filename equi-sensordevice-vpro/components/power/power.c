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
#include "esp_pm.h"

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

static void sleep_pins_setup(void);
static bool s_booting;   /* inside power_boot(): no app tasks running yet */
static bool s_prov_gesture;

bool power_boot_prov_gesture(void) { return s_prov_gesture; }

static void show_charge(power_charge_t c)
{
    led_colour_t col = charge_colour(c);
    status_led_set(col);                                   /* one frame: both LEDs together */
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_SOLID, col);
    status_led_set_mode(LED_IDX_STATUS, LED_MODE_SOLID, col);
    status_led_override_clear();                           /* same colours: no new frame */
}

/*
 * Device "off" on USB: both LEDs show the charge state. The chip stays up in automatic light
 * sleep (RAM and pins kept, ~1-2 mA from USB) instead of deep-sleep polling: every deep-sleep
 * wake is a reboot, and the LEDs visibly blinked on each one.
 */
static void __attribute__((noreturn)) charge_only_sleep(void)
{
    if (!s_booting) {
        /* Called from a running session (power-off on USB): restart so no app task, radio or
         * sensor is left running, and come back here from power_boot(). */
        ESP_LOGI(TAG, "Restarting into charge-only");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_restart();
    }

    sleep_pins_setup();   /* keep the LED / latch pins driven and let the buttons wake us */
    esp_pm_config_t pm = { .max_freq_mhz = 80, .min_freq_mhz = 40, .light_sleep_enable = true };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light sleep unavailable: %s", esp_err_to_name(err));
    }

    wait_btn_release();   /* the press that turned the device off is not a power-on */
    chg_trk_t trk = { .last = -1 };
    power_charge_t shown = (power_charge_t)-1;
    for (;;) {
        power_charge_t c = chg_update(&trk, now_ms());
        if (c != shown) {
            ESP_LOGI(TAG, "Charge-only: %s", charge_name(c));
            show_charge(c);
            shown = c;
        }
        if (btn_raw()) {
            /* Power-on request: the boot code applies the usual hold-to-start rule. */
            ESP_LOGI(TAG, "Button pressed: restarting");
            fflush(stdout);
            esp_restart();
        }
        /* USB removed: nothing to do, the 3.3 V rail drops with it (latch is off). */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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

    /* Scan button (10k external pull-up): configured here, not by the app, because the
     * both-buttons check below runs in every mode (an unconfigured pad reads 0 = "pressed"). */
    gpio_config_t scan = { .pin_bit_mask = BIT64(BOARD_PIN_SCAN_BTN), .mode = GPIO_MODE_INPUT };
    gpio_config(&scan);

    /* PWR_GD (divider) and CHG_IND (100k pull-up) have external pulls. */
    gpio_config_t in = {
        .pin_bit_mask = BIT64(BOARD_PIN_PWR_GD) | BIT64(BOARD_PIN_CHG_IND),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
}

void power_boot(void)
{
    s_booting = true;
    uint32_t causes = esp_sleep_get_wakeup_causes();
    bool usb = power_usb_present();
    bool btn = btn_raw();
    ESP_LOGI(TAG, "Boot: reset=%d wake=0x%lx usb=%d button=%d resume=%d",
             esp_reset_reason(), (unsigned long)causes, usb, btn, s_resume);

    if (s_resume) {
        ESP_LOGW(TAG, "Recovered from reset while running; staying on");
        latch_on();
        s_booting = false;
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
            /* Both buttons held through power-on: the provisioning gesture. Keep white while
             * they stay held; at POWER_PROV_HOLD_MS go straight to provisioning. */
            bool both = false;
            while (btn_raw() && gpio_get_level(BOARD_PIN_SCAN_BTN) == 0) {
                if (now_ms() >= POWER_PROV_HOLD_MS) {
                    both = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (both) {
                ESP_LOGI(TAG, "Both buttons held at power-on: provisioning");
                s_prov_gesture = true;
                status_led_flash(LED_WHITE, 3, 150, 150);
                s_booting = false;
                return;
            }
            status_led_flash(LED_GREEN, 2, 150, 100);
            s_booting = false;
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

static volatile bool s_combo;          /* both buttons held (reported by the app's button task) */
static volatile bool s_shutting_down;

void power_set_combo(bool held) { s_combo = held; }
bool power_shutting_down(void) { return s_shutting_down; }

static void __attribute__((noreturn)) shutdown(bool flash)
{
    s_shutting_down = true;
    ESP_LOGI(TAG, "Shutting down...");
    status_led_set(flash ? LED_ORANGE : LED_OFF);
    for (int i = s_ncbs - 1; i >= 0; i--) {
        s_cbs[i]();
    }
    rails_off();
    /* Stop the "on" breathe / any app pattern; charge-only mode re-applies D2 if on USB. */
    status_led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_OFF);
    status_led_set_mode(LED_IDX_CHARGE, LED_MODE_OFF, LED_OFF);
    if (flash) {
        status_led_flash(LED_ORANGE, 3, 100, 100);
    }
    power_off_now();
}

void power_shutdown(void) { shutdown(true); }
void power_shutdown_silent(void) { shutdown(false); }

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
            bool combo = false;
            while (now_ms() - t0 < POWER_HOLD_OFF_MS) {
                if (s_combo || gpio_get_level(BOARD_PIN_SCAN_BTN) == 0) {
                    combo = true;   /* both buttons: the app's gesture (re-provision), not power-off */
                    break;
                }
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
            if (combo) {
                ESP_LOGI(TAG, "Both buttons held: power-off suppressed");
                while (btn_raw()) {
                    vTaskDelay(pdMS_TO_TICKS(50));   /* wait for release; main handles the gesture */
                }
                continue;
            }
            if (now_ms() - t0 >= POWER_HOLD_OFF_MS) {
                ESP_LOGI(TAG, "Power button held %d ms: powering off", POWER_HOLD_OFF_MS);
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
