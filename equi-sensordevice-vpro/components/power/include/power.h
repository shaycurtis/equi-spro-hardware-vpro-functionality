#pragma once
/*
 * Power sequencing for the SPRO board.
 *
 * Hardware: the 3.3 V rail (and therefore the ESP32 itself) comes from a TPS22810
 * load switch whose enable is a diode-OR of:
 *   - USB/charger present (V_USB divider)
 *   - power button held   (PMIC_BTN)
 *   - PMIC_EN (GPIO18)    (firmware latch)
 * So on battery, "off" means the ESP32 is completely unpowered; pressing the button
 * powers it in hardware. With USB present the rail cannot be cut, so "off" is a
 * charge-only mode showing the charge state on the LEDs.
 *
 * Behaviour:
 *   - Power on:  hold the button for ~1.5 s (POWER_HOLD_ON_MS after firmware start).
 *                Released earlier -> board turns itself off again.
 *   - Running:   the app owns D1 (e.g. breathing blue); D2 shows charge state when USB present
 *                unless the app takes it over with power_set_charge_led(false).
 *   - USB plugged into an off device -> charge-only mode: both LEDs red = charging,
 *                green = full, orange = charger fault; the chip idles in light sleep (no
 *                deep-sleep reboots, so the LEDs never blink). A button hold powers fully on.
 *   - Power off: hold the button for POWER_HOLD_OFF_MS while running. Registered
 *                shutdown callbacks run, rails are switched off, latch released.
 */
#include <stdbool.h>

/* 1: plugging USB in while running on battery ends the session and enters charge-only
 * (booting while already on USB keeps running, for bench work). */
#define POWER_AUTO_STOP_ON_USB 1
#define POWER_HOLD_ON_MS     1200   /* measured from firmware start (~300 ms after press) */
#define POWER_HOLD_OFF_MS    2000
#define POWER_PROV_HOLD_MS   3000   /* both buttons held this long from firmware start = provisioning */

typedef enum {
    POWER_CHARGE_NO_USB = 0,
    POWER_CHARGE_CHARGING,
    POWER_CHARGE_DONE,
    POWER_CHARGE_FAULT,
} power_charge_t;

typedef void (*power_shutdown_cb_t)(void);

/* Call first thing in app_main: puts all power-control pins into a safe state. */
void power_early_init(void);

/*
 * Decide whether to run. Returns only once the power latch is on.
 * Otherwise it powers the board off, or enters charge-only deep sleep (never returns).
 * Requires status_led_init() to have been called.
 */
void power_boot(void);

/* Start the power task (power-button long-press, charge LED). */
void power_start(void);

/* Let the power task show the charge state on D2 (default), or leave D2 to the app. */
void power_set_charge_led(bool enable);

/* Register a callback run (in reverse registration order) during shutdown. */
void power_register_shutdown_cb(power_shutdown_cb_t cb);

/* Graceful shutdown from any task. Never returns. */
void power_shutdown(void) __attribute__((noreturn));

/* Same, without the orange "powering off" flashes (LEDs just go dark). */
void power_shutdown_silent(void) __attribute__((noreturn));

/* Run the shutdown callbacks, then restart with the power latch kept on (the next boot is a
 * "resume", see power_resumed). For a restart from a running device: files are closed and the SD
 * card unmounted first. Never returns. */
void power_restart(void) __attribute__((noreturn));

/* True if this boot followed a reset while the device was running (panic, watchdog, or
 * power_restart / esp_restart with the latch on), not a power-on. Valid after power_early_init. */
bool power_resumed(void);

/* True if the device was powered on with both buttons held (the provisioning gesture). */
bool power_boot_prov_gesture(void);

/* The app's button task reports while both buttons are held (its gesture), so a power-button
 * hold that is part of that gesture never powers the device off. */
void power_set_combo(bool held);

/* True once power_shutdown() has started: button gestures must be ignored from then on. */
bool power_shutting_down(void);

bool power_usb_present(void);
power_charge_t power_charge_state(void);

/* Charge state read from the pins now (no fault tracking); usable before power_start(). */
power_charge_t power_charge_now(void);

/* Called once when the device enters charge-only mode on USB (from power_boot, before any app
 * task runs), e.g. to start the Wi-Fi / backend work. Must return quickly (start a task).
 * Register before power_boot(). */
typedef void (*power_charge_only_hook_t)(void);
void power_set_charge_only_hook(power_charge_only_hook_t fn);

/* Charge-only: true = D2 shows something else (the Wi-Fi state); the charge colour is then only
 * on D1, the more visible LED. false = both LEDs show the charge colour again. */
void power_charge_only_claim_status_led(bool claimed);
