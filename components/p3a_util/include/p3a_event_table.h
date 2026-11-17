// SPDX-License-Identifier: Apache-2.0
// The event bus's subscriber table, kept apart from the bus so that it can be
// tested on the host. Not thread-safe: the bus serializes access.
// Host-testable.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "p3a_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_EVENT_MAX_SUBSCRIBERS 48

typedef struct {
    uint16_t type;
    p3a_event_handler_t handler;
    void *ctx;
} p3a_event_sub_t;

typedef struct {
    p3a_event_sub_t subs[P3A_EVENT_MAX_SUBSCRIBERS];
    size_t count;
} p3a_event_table_t;

void p3a_event_table_init(p3a_event_table_t *table);

// ESP_ERR_INVALID_ARG for a NULL handler, ESP_ERR_INVALID_STATE when this
// exact registration exists already, ESP_ERR_NO_MEM when the table is full.
esp_err_t p3a_event_table_add(p3a_event_table_t *table, uint16_t type, p3a_event_handler_t handler, void *ctx);

// Removes one registration, keeping the order of the rest. False when it was
// not there.
bool p3a_event_table_remove(p3a_event_table_t *table, uint16_t type, p3a_event_handler_t handler, void *ctx);

// Copies the registrations for `type`, in subscription order, into out[].
// Returns how many were copied (at most max).
size_t p3a_event_table_match(const p3a_event_table_t *table, uint16_t type, p3a_event_sub_t *out, size_t max);

#ifdef __cplusplus
}
#endif
