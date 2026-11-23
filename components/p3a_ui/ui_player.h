// SPDX-License-Identifier: Apache-2.0
// What the screen should say about playback, worked out from the player's
// state. Pure: host-tested (tests/host/ui/test_ui_player.c). Private to the
// UI.
//
// Nobody tells the UI "now show Loading". The scheduler says what is true
// (these channels are being listed, nothing is playable, that request
// failed) and the notices follow. In 1.x four writers called
// set_channel_message() and an arbitration rule decided which call won.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "p3a_ui_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

// The player's state, reduced to what the notices depend on.
typedef struct {
    bool playing;         // an artwork is on the panel
    bool loading;         // a request is with the player
    uint32_t channels;    // in the active playset
    uint32_t listing;     // of them, not yet listed
    uint32_t failed;      // of them, could not be listed
    uint32_t no_source;   // of them, from a source this firmware lacks
    uint32_t playable;    // artworks that could be chosen now
    bool card_failed;     // a channel could not be listed because of the card
    int problem;          // why the last thing a person asked for was not shown; 0 for none
    uint32_t problems;    // how many such since boot
} ui_player_facts_t;

typedef enum {
    UI_PLAYER_NONE = 0,   // no notice: something plays, or will in a moment
    UI_PLAYER_LOADING,
    UI_PLAYER_NOTHING,
} ui_player_standing_t;

// Which standing notice the facts call for, and its words.
ui_player_standing_t ui_player_standing(const ui_player_facts_t *facts, p3a_ui_content_t *out);

// The words for a request that could not be shown.
void ui_player_problem(int problem, p3a_ui_content_t *out);

#define UI_PLAYER_PROBLEM_TTL_MS   8000
#define UI_PLAYER_LOADING_AFTER_MS 1500  // how long the panel may be dark before "getting ready" is said

#ifdef __cplusplus
}
#endif
