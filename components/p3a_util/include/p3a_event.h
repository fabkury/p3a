// SPDX-License-Identifier: Apache-2.0
// The event bus.
//
// WHAT IT IS FOR. Fan-out: one thing happened, and several parts of the
// firmware that know nothing of each other need to hear about it. Wi-Fi came
// up. A swap was requested. The SD card failed. Memory is getting tight.
//
// WHAT IT IS NOT FOR. Calling one known function. If the emitter knows who
// should act, it calls that code directly when it is in a lower layer, or
// goes through the hook table that main fills when it is in a higher one.
// An event with exactly one subscriber, forever, is a function call wearing
// a costume; write the call.
//
// HOW IT BEHAVES.
//  - Emitting never blocks. The event is copied into a queue and the call
//    returns. A full queue drops the event and reports it (see
//    p3a_event_dropped); emitters that must not lose a message need another
//    mechanism.
//  - Handlers run later, one event at a time, on the bus's own task, in the
//    order they subscribed. No lock is held while a handler runs, so a
//    handler may emit, subscribe and unsubscribe freely.
//  - A handler must be quick: it delays every event behind it. Slow work
//    goes to a job (p3a_job.h).
//  - An event carries a type and one small payload. Anything larger lives
//    with its owner and is fetched by the handler.
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "p3a_event_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t type;          // a p3a_event_type_t
    uint32_t timestamp_ms;  // time since boot when it was emitted
    union {
        int32_t i32;
        uint32_t u32;
        void *ptr;          // must stay valid until every handler has run
    } payload;
} p3a_event_t;

typedef void (*p3a_event_handler_t)(const p3a_event_t *event, void *ctx);

// Starts the bus. Call once, early in app_main. Safe to call again.
esp_err_t p3a_event_bus_start(void);

// Registers a handler for one event type. The same handler and ctx can be
// registered once per type. ESP_ERR_NO_MEM when the table is full.
esp_err_t p3a_event_subscribe(uint16_t type, p3a_event_handler_t handler, void *ctx);

// Removes a registration. An event already being delivered may still reach
// the handler once.
void p3a_event_unsubscribe(uint16_t type, p3a_event_handler_t handler, void *ctx);

// Emit. Callable from any task, not from an interrupt. ESP_ERR_TIMEOUT when
// the queue is full and the event was dropped.
esp_err_t p3a_event_emit(uint16_t type);
esp_err_t p3a_event_emit_i32(uint16_t type, int32_t value);
esp_err_t p3a_event_emit_ptr(uint16_t type, void *ptr);

// How many events have been dropped on a full queue since boot.
uint32_t p3a_event_dropped(void);

#ifdef __cplusplus
}
#endif
