/* RAM ring of finished bursts: published in order, the oldest dropped when full (offline). */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gw.h"

static const char *TAG = "queue";
static gw_burst_t *s_ring;
static int s_head, s_count;
static uint32_t s_dropped;
static uint32_t s_head_id;     /* running number of the burst at the head */
static SemaphoreHandle_t s_lock;

void gw_queue_init(void)
{
    s_ring = heap_caps_calloc(CONFIG_GW_QUEUE_LEN, sizeof(gw_burst_t), MALLOC_CAP_8BIT);
    assert(s_ring);
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "%d bursts (%u bytes)", CONFIG_GW_QUEUE_LEN, (unsigned)(CONFIG_GW_QUEUE_LEN * sizeof(gw_burst_t)));
}

void gw_queue_push(const gw_burst_t *b)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == CONFIG_GW_QUEUE_LEN) {
        s_head = (s_head + 1) % CONFIG_GW_QUEUE_LEN;   /* drop the oldest */
        s_count--;
        s_head_id++;
        s_dropped++;
    }
    s_ring[(s_head + s_count) % CONFIG_GW_QUEUE_LEN] = *b;
    s_count++;
    xSemaphoreGive(s_lock);
}

bool gw_queue_peek(gw_burst_t *out, uint32_t *id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_count > 0;
    if (ok) {
        *out = s_ring[s_head];
        *id = s_head_id;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/* Pops only the burst that was peeked (it may have been dropped meanwhile). */
void gw_queue_pop(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count > 0 && id == s_head_id) {
        s_head_id++;
        s_head = (s_head + 1) % CONFIG_GW_QUEUE_LEN;
        s_count--;
    }
    xSemaphoreGive(s_lock);
}

int gw_queue_count(void)
{
    return s_count;
}

uint32_t gw_queue_dropped(void)
{
    return s_dropped;
}
