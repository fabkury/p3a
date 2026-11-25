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
    // A setting has a new value, already saved. One event per setting.
    // payload.i32: its p3a_setting_id_t. Read the value with p3a_settings_get().
    P3A_EVENT_SETTING_CHANGED,

    // --- sources 0x0400: none yet ---

    // --- library (0x0500) ---
    // The pin lists have changed: an artwork pinned or unpinned, a list
    // made, renamed, deleted or made the active one. No payload: ask
    // p3a_pins.h.
    P3A_EVENT_PINS_CHANGED = 0x0500,

    // --- player (0x0600) ---
    // Something about playback that others show or report has changed: what
    // is playing, a channel's state, pause, a request that failed. No
    // payload: ask play_scheduler_get_status().
    P3A_EVENT_PLAYER_CHANGED = 0x0600,

    // --- ui 0x0700: none yet ---

    // --- net (0x0800) ---
    // The device has joined Wi-Fi and has an address. No payload.
    P3A_EVENT_NET_UP = 0x0800,
    // The device had an address and no longer has one. Not emitted for a
    // failed attempt while it was already down. No payload.
    P3A_EVENT_NET_DOWN,
    // The device has no network to join and has opened its setup access
    // point. It stays there until it is restarted. No payload.
    P3A_EVENT_NET_SETUP_STARTED,

    // --- test hooks (0xFF00): never emitted by release code ---
    P3A_EVENT_TEST_A = 0xFF00,
    P3A_EVENT_TEST_B,
} p3a_event_type_t;

#ifdef __cplusplus
}
#endif
