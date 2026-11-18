// SPDX-License-Identifier: Apache-2.0
// Every event type on the bus, in one place, so that the vocabulary can be
// read at a glance. An event is added here by the milestone that brings its
// emitter, with what it means and what its payload is. Read p3a_event.h
// first: most things are not events.
//
// Each layer owns a block of 0x100 values.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_EVENT_NONE = 0,

    // --- util (0x0100) ---
    // The internal DMA-capable memory pool crossed a pressure threshold.
    // payload.i32: the new p3a_mem_level_t.
    P3A_EVENT_MEM_PRESSURE_CHANGED = 0x0100,

    // --- board (0x0200): none yet ---

    // --- storage (0x0300) ---
    // The SD card has been declared failed for the rest of this boot: a boot
    // probe failed, or three writes in a row did. Nothing may write to it any
    // more. Emitted once. No payload.
    P3A_EVENT_SD_HEALTH_FAILED = 0x0300,

    // --- sources 0x0400, library 0x0500, player 0x0600, ui 0x0700,
    //     net 0x0800: none yet ---

    // --- test hooks (0xFF00): never emitted by release code ---
    P3A_EVENT_TEST_A = 0xFF00,
    P3A_EVENT_TEST_B,
} p3a_event_type_t;

#ifdef __cplusplus
}
#endif
