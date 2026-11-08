#include "device_id.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "device_id";
static char s_id[24];

static bool read_from_nvs(void)
{
    /* Deliberately no erase-on-error: NVS holds the SPro device ID, passkey and settings. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s); not erasing", esp_err_to_name(err));
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("system", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_id);
    err = nvs_get_str(h, "device_id", s_id, &len);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        uint32_t v;
        if (nvs_get_u32(h, "device_id", &v) == ESP_OK) {
            snprintf(s_id, sizeof(s_id), "%06" PRIX32, v);
            err = ESP_OK;
        }
    }
    nvs_close(h);
    return err == ESP_OK && s_id[0];
}

const char *device_id_get(void)
{
    if (s_id[0]) {
        return s_id;
    }
    if (read_from_nvs()) {
        ESP_LOGI(TAG, "using SPro device ID from NVS: %s", s_id);
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_id, sizeof(s_id), "%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGW(TAG, "no device ID in NVS, using MAC-derived ID: %s", s_id);
    }
    return s_id;
}
