// SPDX-License-Identifier: Apache-2.0
// The scheduler: decides what plays next and asks the animation player for it.
//
//   the active playset  ->  its channels, each an index listed by its source
//   next                ->  a channel (by weight), an artwork of it, the player
//   the player's report ->  "shown": the artwork enters the history
//                           "failed": the turn is taken back
//   back                ->  the artwork shown before, from the history
//
// It names no source: every channel goes through the table its source
// registered (p3a_source.h). When to move on (the dwell timer), what to do
// about a failure beyond taking the turn back, and pause are the loader
// pipeline's (M2.4); here "next" and "back" happen when someone asks.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "animation_player.h"
#include "esp_err.h"
#include "p3a_record.h"
#include "playset.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the scheduler: takes over the animation player's reports, loads the
// active playset from the card (or, if there is none, a playset of the
// user's own files), lists its channels in the background and then plays.
// Call once at boot, after the player, storage and the sources.
esp_err_t play_scheduler_start(void);

// Makes a playset the active one, saves it as such, and lists its channels
// in the background; playing starts when the first of them has something.
// The history is kept. ESP_ERR_INVALID_ARG for a playset that fails its check.
esp_err_t play_scheduler_activate(const playset_t *playset);

// Plays the next artwork: forward in the history if "back" was used, else a
// new pick. ESP_ERR_NOT_FOUND when no channel has anything playable.
esp_err_t play_scheduler_next(void);

// Plays the artwork shown before the current one. ESP_ERR_NOT_FOUND at the
// far end of the history.
esp_err_t play_scheduler_back(void);

// Lists a source's channels of the active playset again, in the background:
// for when something says they changed (an upload, a USB session).
esp_err_t play_scheduler_refresh_source(uint8_t source);

typedef enum {
    PLAY_CHANNEL_WAITING = 0,  // not listed yet
    PLAY_CHANNEL_LISTING,
    PLAY_CHANNEL_READY,
    PLAY_CHANNEL_FAILED,       // its source could not list it; see `error`
    PLAY_CHANNEL_NO_SOURCE,    // this firmware has no such source
    PLAY_CHANNEL_NOT_LOADED,   // beyond the number of channels held in memory at once
} play_channel_state_t;

typedef struct {
    char id[PLAYSET_CHANNEL_ID_LEN];
    uint8_t source;
    uint8_t state;      // play_channel_state_t
    esp_err_t error;    // of the last listing
    uint32_t records;
    uint32_t available;
} play_channel_status_t;

typedef struct {
    char playset[PLAYSET_NAME_LEN];
    uint32_t channel_count;
    play_channel_status_t channels[PLAYSET_MAX_CHANNELS];
    bool playing;           // an artwork is on the panel
    p3a_record_t current;   // it, when `playing`
    bool loading;           // a request is with the player
    uint32_t history_count, history_position;
    uint32_t picks, shown, failed;  // since boot
    esp_err_t last_error;   // of the last failed request
} play_scheduler_status_t;  // about 2 KB: allocate it

void play_scheduler_get_status(play_scheduler_status_t *out);

// A copy of the active playset.
void play_scheduler_get_playset(playset_t *out);

// The scheduler is the animation player's listener. Something else that
// asks the player for a file directly (the test hooks) hears about its own
// requests through this instead: reports for tokens the scheduler did not
// issue are passed on. Such tokens must be below 0x80000000.
void play_scheduler_set_foreign_listener(const animation_player_listener_t *listener, void *ctx);

#ifdef __cplusplus
}
#endif
