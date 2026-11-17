// SPDX-License-Identifier: Apache-2.0
#include "p3a_event.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_event_table.h"
#include "p3a_task.h"

#define QUEUE_LENGTH 32
// Every handler runs on this stack. 4 KB overflowed in 1.x when a handler hit
// a deep error path during an SD fault storm; 8 KB has held since.
#define DISPATCH_STACK_BYTES 8192

static const char *TAG = "p3a_event";

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;  // guards s_table only; never held while a handler runs
static p3a_event_table_t s_table;
static atomic_uint_least32_t s_dropped;
static bool s_started;

static void dispatch_task(void *arg)
{
    (void)arg;
    // Static: the dispatch task is the only user, and 48 entries would
    // otherwise sit on its stack beside every handler's frames.
    static p3a_event_sub_t matched[P3A_EVENT_MAX_SUBSCRIBERS];

    for (;;) {
        p3a_event_t event;
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t n = p3a_event_table_match(&s_table, event.type, matched, P3A_EVENT_MAX_SUBSCRIBERS);
        xSemaphoreGive(s_lock);

        for (size_t i = 0; i < n; i++) {
            matched[i].handler(&event, matched[i].ctx);
        }
    }
}

esp_err_t p3a_event_bus_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(p3a_event_t));
    s_lock = xSemaphoreCreateMutex();
    if (!s_queue || !s_lock) {
        return ESP_ERR_NO_MEM;
    }
    p3a_event_table_init(&s_table);

    p3a_task_config_t task = P3A_TASK_CONFIG("p3a_events", dispatch_task, NULL, DISPATCH_STACK_BYTES, P3A_TASK_APP);
    esp_err_t err = p3a_task_create(&task, NULL);
    if (err != ESP_OK) {
        return err;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t p3a_event_subscribe(uint16_t type, p3a_event_handler_t handler, void *ctx)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = p3a_event_table_add(&s_table, type, handler, ctx);
    xSemaphoreGive(s_lock);
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "subscriber table full (%d); event 0x%04x not subscribed", P3A_EVENT_MAX_SUBSCRIBERS, type);
    }
    return err;
}

void p3a_event_unsubscribe(uint16_t type, p3a_event_handler_t handler, void *ctx)
{
    if (!s_started) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    p3a_event_table_remove(&s_table, type, handler, ctx);
    xSemaphoreGive(s_lock);
}

static esp_err_t emit(p3a_event_t *event)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    event->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (xQueueSend(s_queue, event, 0) != pdTRUE) {
        uint32_t dropped = atomic_fetch_add(&s_dropped, 1) + 1;
        // Say so on the first drop and then on every 64th: a full queue
        // usually means a stuck handler, and logging each drop would not help.
        if (dropped == 1 || dropped % 64 == 0) {
            ESP_LOGW(TAG, "queue full: event 0x%04x dropped (%u dropped since boot)", event->type, (unsigned)dropped);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t p3a_event_emit(uint16_t type)
{
    p3a_event_t event = {.type = type};
    return emit(&event);
}

esp_err_t p3a_event_emit_i32(uint16_t type, int32_t value)
{
    p3a_event_t event = {.type = type, .payload.i32 = value};
    return emit(&event);
}

esp_err_t p3a_event_emit_ptr(uint16_t type, void *ptr)
{
    p3a_event_t event = {.type = type, .payload.ptr = ptr};
    return emit(&event);
}

uint32_t p3a_event_dropped(void)
{
    return atomic_load(&s_dropped);
}
