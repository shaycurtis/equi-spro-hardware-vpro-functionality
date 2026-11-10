#pragma once
/*
 * External temperature probe (the horse temperature): a DS18B20 on the 1-Wire bus of the
 * 9-pin magnetic connector (MC1):
 *   pin 4  CON_1WIRE -> USBLC6 (U15) -> GPIO12, 4.7k pull-up to VDD33 on the board (R38)
 *   pin 5  CON_3V3   =  VDD33 through a 50 mA PTC fuse (F1): the probe's VDD (powered mode)
 *   pin 8  GND
 * VDD33 is off whenever the device is off, so the probe draws nothing then.
 *
 * Uses Espressif's onewire_bus (RMT backend) and ds18b20 components, unmodified. The RMT
 * channels hold a power-management lock (CPU at max) for as long as the bus exists, which would
 * stop light sleep, so the bus is created only around each transfer (a few ms). The conversion
 * (up to 750 ms at 12 bits) runs inside the sensor between readings while the ESP32 sleeps:
 * probe_read() returns the result of the conversion started by the previous call, then starts
 * the next one, so each value is up to one reading period (60 s) old.
 *
 * Plugging the probe in or out is detected at every call.
 *
 * Call both functions from core 0 (app_main / a task pinned to core 0): RMT channels share one
 * interrupt, which the status LEDs' RMT channel holds on core 0, and a new channel can only join
 * it from the same core ("install rx interrupt failed" otherwise).
 */
#include "esp_err.h"

/* Look for a probe and start its first conversion. Never fails: no probe is a normal state. */
void probe_init(void);

/*
 * Temperature in degC from the conversion started at the previous call; then start the next.
 *   ESP_OK                 *temp_c is valid
 *   ESP_ERR_NOT_FOUND      no probe on the bus
 *   ESP_ERR_INVALID_STATE  probe present but no finished conversion yet (just plugged in, boot)
 *   other                  read failed (e.g. ESP_ERR_INVALID_CRC: noisy cable)
 */
esp_err_t probe_read(float *temp_c);
