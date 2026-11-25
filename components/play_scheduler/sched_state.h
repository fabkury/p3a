// SPDX-License-Identifier: Apache-2.0
// The scheduler's state, shared by its two files that do the wiring:
// play_scheduler.c (the player, the timer, what plays next) and
// sched_content.c (the task that lists channels and downloads artworks).
// Private to the component.
//
// One recursive lock guards everything here. It is held for arithmetic and
// for handing a request to the player, never across the card or the
// network.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "animation_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_index.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "play_scheduler.h"
#include "playset.h"
#include "sched_core.h"
#include "sched_history.h"
#include "sched_pick.h"
#include "sched_refresh.h"
#include "sched_turn.h"

// Indices held in memory at once. A playset may name more channels; the
// rest are not loaded.
#define SCHED_RESIDENT_MAX     16
#define SCHED_DEFAULT_CAPACITY 1024

// What a request is for. NEW was picked from a channel; the others name one
// artwork. NEW and SHOW enter the history when shown.
typedef enum { TURN_NEW, TURN_BACK, TURN_FORWARD, TURN_SHOW } turn_kind_t;

typedef struct {
    p3a_index_t index;
    bool has_index;
    uint8_t state;  // play_channel_state_t
    esp_err_t error;
    char id[PLAYSET_CHANNEL_ID_LEN];
    // The content task's.
    sched_refresh_t refresh;  // when it is listed again
    bool load_tried;          // its index was looked for on the card
    bool dirty;               // the index holds something its file does not
    int64_t save_at_ms;       // when a dirty index is written
    uint32_t cursor;          // where the search for the next artwork to download starts
    uint32_t listings;        // finished, whatever the result
} sched_slot_t;

typedef struct {
    playset_t playset;
    sched_slot_t channels[PLAYSET_MAX_CHANNELS];
    uint32_t generation;  // changes with every activation; work begun for an older one is dropped
    sched_core_t core;
    sched_history_t history;
    sched_recent_t recent;
    sched_turn_t turn;
    bool paused;
    bool shut_down;          // the card is being taken away; nothing more is played
    bool advance_queued;     // the job that moves on is waiting to run
    esp_err_t problem;       // why the last turn a person asked for showed nothing; ESP_OK once something is shown
    uint32_t problems;

    // The request that is with the player, if any.
    uint32_t pending_token;
    turn_kind_t pending_kind;
    p3a_record_t pending_record;
    uint32_t pending_channel;
    uint32_t pending_generation;
    int64_t pending_since_ms;
    bool pending_overdue_said;  // the warning below was logged for this request

    bool playing;
    p3a_record_t current;
    bool foreign_shown;      // a picture that is not the scheduler's is on the panel, or about to be
    bool staged;             // the pending request is loaded and waits for the display to draw it
    uint32_t foreign_token;  // the foreign request last reported ready with a picture
    uint32_t next_token;
    uint32_t picks, shown, failed;
    esp_err_t last_error;
    bool start_wanted;  // a new playset is in place and nothing of it has been shown yet

    bool downloading;   // a download is in progress
    uint32_t downloads, download_failures;  // since boot

    animation_player_listener_t foreign;
    void *foreign_ctx;
    p3a_settings_t settings;  // scratch for reading the settings; too large for a stack
} sched_state_t;

extern sched_state_t *sched;  // in PSRAM: about 40 KB
extern SemaphoreHandle_t sched_lock;

#define LOCK()   xSemaphoreTakeRecursive(sched_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(sched_lock)

// --- play_scheduler.c ---

int64_t sched_now_ms(void);

// Tells whoever shows or reports the player's state that it changed.
void sched_changed(void);

// Something became playable: starts playing if nothing is, or if a new
// playset waits for its first artwork. Called with the lock held.
void sched_start_if_wanted(void);

// --- sched_content.c ---

esp_err_t sched_content_start(void);

// Whether the network is up, as the content task last heard.
bool sched_content_online(void);

// How many of an index's records the content task will download.
uint32_t sched_content_wanted(const p3a_index_t *index);

// There may be work: a playset was put in place, a listing was asked for.
void sched_content_wake(void);

// Waits until the content task is between two pieces of work; false if it
// did not get there in time. With `shut_down` set it then starts no more.
bool sched_content_wait_idle(uint32_t timeout_ms);
