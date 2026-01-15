// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#define EVENT_QUEUE_SIZE 32
#define MAX_SUBSCRIBERS 48
#define EVENT_TYPE_CATEGORY_ALL 0xFFFF

typedef struct {
    uint16_t event_type;
    uint16_t category;
    p3a_event_handler_t handler;
    void *ctx;
} subscriber_t;

typedef struct {
    QueueHandle_t queue;
    subscriber_t subscribers[MAX_SUBSCRIBERS];
    size_t subscriber_count;
    TaskHandle_t dispatch_task;
    SemaphoreHandle_t mutex;
    bool initialized;
} event_bus_state_t;

static const char *TAG = "event_bus";
static event_bus_state_t s_bus = {0};

static uint16_t event_type_to_category(uint16_t type)
{
    switch (type) {
        case P3A_EVENT_WIFI_CONNECTED:
        case P3A_EVENT_WIFI_DISCONNECTED:
        case P3A_EVENT_MQTT_CONNECTED:
        case P3A_EVENT_MQTT_DISCONNECTED:
        case P3A_EVENT_REGISTRATION_CHANGED:
        case P3A_EVENT_MAKAPIX_STATE_CHANGED:
            return P3A_EVENT_CATEGORY_SYSTEM;
        case P3A_EVENT_SWAP_NEXT:
        case P3A_EVENT_SWAP_BACK:
        case P3A_EVENT_PAUSE:
        case P3A_EVENT_RESUME:
            return P3A_EVENT_CATEGORY_PLAYBACK;
        case P3A_EVENT_PROVISIONING_STATUS_CHANGED:
            return P3A_EVENT_CATEGORY_UI;
        default:
            return P3A_EVENT_CATEGORY_SYSTEM;
    }
}

static void event_bus_dispatch_task(void *arg)
{
    (void)arg;
    p3a_event_t event;

    while (true) {
        if (xQueueReceive(s_bus.queue, &event, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
            for (size_t i = 0; i < s_bus.subscriber_count; i++) {
                subscriber_t *sub = &s_bus.subscribers[i];
                bool match = (sub->event_type == event.type) ||
                             (sub->event_type == EVENT_TYPE_CATEGORY_ALL &&
                              sub->category == event.category);
                if (match && sub->handler) {
                    sub->handler(&event, sub->ctx);
                }
            }
            xSemaphoreGive(s_bus.mutex);
        }
    }
}

esp_err_t event_bus_init(void)
{
    if (s_bus.initialized) {
        return ESP_OK;
    }

    s_bus.queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(p3a_event_t));
    if (!s_bus.queue) {
        return ESP_ERR_NO_MEM;
    }

    s_bus.mutex = xSemaphoreCreateMutex();
    if (!s_bus.mutex) {
        vQueueDelete(s_bus.queue);
        s_bus.queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(event_bus_dispatch_task, "event_bus", 4096, NULL, 5, &s_bus.dispatch_task);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_bus.mutex);
        s_bus.mutex = NULL;
        vQueueDelete(s_bus.queue);
        s_bus.queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_bus.subscriber_count = 0;
    s_bus.initialized = true;
    ESP_LOGI(TAG, "Event bus initialized");
    return ESP_OK;
}

esp_err_t event_bus_subscribe(uint16_t event_type, p3a_event_handler_t handler, void *ctx)
{
    if (!s_bus.initialized || !handler) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
    if (s_bus.subscriber_count >= MAX_SUBSCRIBERS) {
        xSemaphoreGive(s_bus.mutex);
        return ESP_ERR_NO_MEM;
    }

    subscriber_t *sub = &s_bus.subscribers[s_bus.subscriber_count++];
    sub->event_type = event_type;
    sub->category = event_type_to_category(event_type);
    sub->handler = handler;
    sub->ctx = ctx;

    xSemaphoreGive(s_bus.mutex);
    return ESP_OK;
}

esp_err_t event_bus_subscribe_category(uint16_t category, p3a_event_handler_t handler, void *ctx)
{
    if (!s_bus.initialized || !handler) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
    if (s_bus.subscriber_count >= MAX_SUBSCRIBERS) {
        xSemaphoreGive(s_bus.mutex);
        return ESP_ERR_NO_MEM;
    }

    subscriber_t *sub = &s_bus.subscribers[s_bus.subscriber_count++];
    sub->event_type = EVENT_TYPE_CATEGORY_ALL;
    sub->category = category;
    sub->handler = handler;
    sub->ctx = ctx;

    xSemaphoreGive(s_bus.mutex);
    return ESP_OK;
}

void event_bus_unsubscribe(p3a_event_handler_t handler)
{
    if (!s_bus.initialized || !handler) {
        return;
    }

    xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_bus.subscriber_count; i++) {
        if (s_bus.subscribers[i].handler == handler) {
            for (size_t j = i; j < s_bus.subscriber_count - 1; j++) {
                s_bus.subscribers[j] = s_bus.subscribers[j + 1];
            }
            s_bus.subscriber_count--;
            break;
        }
    }
    xSemaphoreGive(s_bus.mutex);
}

static esp_err_t event_bus_emit_internal(uint16_t event_type, p3a_event_t *event)
{
    if (!s_bus.initialized || !s_bus.queue || !event) {
        return ESP_ERR_INVALID_STATE;
    }

    event->type = event_type;
    if (event->category == 0) {
        event->category = event_type_to_category(event_type);
    }
    event->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    return (xQueueSend(s_bus.queue, event, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t event_bus_emit(uint16_t event_type, const p3a_event_t *event)
{
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    p3a_event_t copy = *event;
    return event_bus_emit_internal(event_type, &copy);
}

esp_err_t event_bus_emit_simple(uint16_t event_type)
{
    p3a_event_t event = {0};
    return event_bus_emit_internal(event_type, &event);
}

esp_err_t event_bus_emit_i32(uint16_t event_type, int32_t value)
{
    p3a_event_t event = {0};
    event.payload.i32 = value;
    return event_bus_emit_internal(event_type, &event);
}

esp_err_t event_bus_emit_ptr(uint16_t event_type, void *ptr)
{
    p3a_event_t event = {0};
    event.payload.ptr = ptr;
    return event_bus_emit_internal(event_type, &event);
}
