#include "probe.h"
#include <string.h>
#include "board.h"
#include "ds18b20.h"
#include "onewire_bus.h"
#include "onewire_cmd.h"
#include "onewire_device.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "probe";

#define DS18B20_FAMILY     0x28
#define DS18B20_CONVERT_T  0x44
#define CONVERSION_US      (750 * 1000)   /* 12-bit conversion, DS18B20 datasheet max */

static bool s_present;                    /* a DS18B20 was found */
static onewire_device_address_t s_addr;   /* its ROM ID (family code in the low byte) */
static int64_t s_started_us;              /* when its pending conversion started; 0 = none */

static esp_err_t bus_open(onewire_bus_handle_t *bus)
{
    onewire_bus_config_t cfg = {
        .bus_gpio_num = BOARD_PIN_1WIRE,
        .flags.en_pull_up = false,        /* R38, 4.7k on the board */
    };
    onewire_bus_rmt_config_t rmt = { .max_rx_bytes = 10 };   /* 8-byte ROM ID, 9-byte scratchpad */
    return onewire_new_bus_rmt(&cfg, &rmt, bus);
}

/* Search the bus for the first DS18B20. */
static bool find_probe(onewire_bus_handle_t bus)
{
    if (onewire_bus_reset(bus) != ESP_OK) {   /* no presence pulse: nothing plugged in (quietly) */
        return false;
    }
    onewire_device_iter_handle_t it;
    if (onewire_new_device_iter(bus, &it) != ESP_OK) {
        return false;
    }
    onewire_device_t dev;
    bool found = false;
    while (!found && onewire_device_iter_get_next(it, &dev) == ESP_OK) {
        if ((dev.address & 0xFF) == DS18B20_FAMILY) {
            s_addr = dev.address;
            found = true;
        } else {
            ESP_LOGW(TAG, "1-Wire device %016llX is not a DS18B20, ignored", (unsigned long long)dev.address);
        }
    }
    onewire_del_device_iter(it);
    return found;
}

/* Convert T to our probe only (Match ROM), without waiting: the ds18b20 component's trigger
 * waits up to 800 ms with the bus (and its power lock) held. Same bytes as that function. */
static esp_err_t start_conversion(onewire_bus_handle_t bus)
{
    esp_err_t err = onewire_bus_reset(bus);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t tx[10] = { ONEWIRE_CMD_MATCH_ROM };
    memcpy(&tx[1], &s_addr, sizeof(s_addr));
    tx[9] = DS18B20_CONVERT_T;
    return onewire_bus_write_bytes(bus, tx, sizeof(tx));
}

static void lost(esp_err_t why)
{
    if (s_present) {
        ESP_LOGW(TAG, "probe %016llX gone (%s)", (unsigned long long)s_addr, esp_err_to_name(why));
    }
    s_present = false;
    s_started_us = 0;
}

esp_err_t probe_read(float *temp_c)
{
    onewire_bus_handle_t bus;
    esp_err_t err = bus_open(&bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "1-Wire bus: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_present && find_probe(bus)) {
        s_present = true;
        s_started_us = 0;
        ESP_LOGI(TAG, "DS18B20 %016llX on the 1-Wire bus", (unsigned long long)s_addr);
    }
    err = ESP_ERR_NOT_FOUND;
    if (s_present) {
        onewire_device_t dev = { .bus = bus, .address = s_addr };
        ds18b20_config_t cfg = {};               /* no options in this version */
        ds18b20_device_handle_t ds;
        err = ds18b20_new_device_from_enumeration(&dev, &cfg, &ds);
        if (err == ESP_OK) {
            if (s_started_us && esp_timer_get_time() - s_started_us >= CONVERSION_US) {
                err = ds18b20_get_temperature(ds, temp_c);
            } else {
                err = ESP_ERR_INVALID_STATE;     /* nothing converted yet */
            }
            ds18b20_del_device(ds);
            if (err == ESP_ERR_NOT_FOUND) {      /* no presence pulse: unplugged */
                lost(err);
            } else {
                esp_err_t e = start_conversion(bus);
                if (e == ESP_OK) {
                    s_started_us = esp_timer_get_time();
                } else {
                    lost(e);
                    err = e;
                }
            }
        }
    }
    onewire_bus_del(bus);                        /* releases the RMT channels and their power lock */
    return err;
}

void probe_init(void)
{
    float t;
    probe_read(&t);                              /* detect + start the first conversion */
    if (!s_present) {
        ESP_LOGI(TAG, "no DS18B20 on the 1-Wire bus (connector pin 4); checked at every reading");
    }
}
