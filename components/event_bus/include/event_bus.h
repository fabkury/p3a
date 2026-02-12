// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_EVENT_CATEGORY_SYSTEM = 1,
    P3A_EVENT_CATEGORY_CONTENT = 2,
    P3A_EVENT_CATEGORY_PLAYBACK = 3,
    P3A_EVENT_CATEGORY_UI = 4,
} p3a_event_category_t;

typedef enum {
    // System events
    P3A_EVENT_WIFI_CONNECTED = 100,
    P3A_EVENT_WIFI_DISCONNECTED,
    P3A_EVENT_MQTT_CONNECTED,
    P3A_EVENT_MQTT_DISCONNECTED,
    P3A_EVENT_REGISTRATION_CHANGED,
    P3A_EVENT_INTERNET_CHECK,
    P3A_EVENT_MAKAPIX_STATE_CHANGED,

    // Content events
    P3A_EVENT_CACHE_FLUSH = 150,

    // Playback events
    P3A_EVENT_SWAP_NEXT = 200,
    P3A_EVENT_SWAP_BACK,
    P3A_EVENT_PAUSE,
    P3A_EVENT_RESUME,
    P3A_EVENT_TOGGLE_PAUSE,

    // UI events
    P3A_EVENT_PROVISIONING_STATUS_CHANGED = 300,
} p3a_event_type_t;

typedef struct {
    uint16_t type;
    uint16_t category;
    uint32_t timestamp_ms;
    union {
        int32_t i32;
        uint32_t u32;
        void *ptr;
    } payload;
} p3a_event_t;

typedef void (*p3a_event_handler_t)(const p3a_event_t *event, void *ctx);

esp_err_t event_bus_init(void);
esp_err_t event_bus_subscribe(uint16_t event_type, p3a_event_handler_t handler, void *ctx);
esp_err_t event_bus_subscribe_category(uint16_t category, p3a_event_handler_t handler, void *ctx);
void event_bus_unsubscribe(p3a_event_handler_t handler);

esp_err_t event_bus_emit(uint16_t event_type, const p3a_event_t *event);
esp_err_t event_bus_emit_simple(uint16_t event_type);
esp_err_t event_bus_emit_i32(uint16_t event_type, int32_t value);
esp_err_t event_bus_emit_ptr(uint16_t event_type, void *ptr);

#ifdef __cplusplus
}
#endif
