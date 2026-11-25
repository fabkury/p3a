// SPDX-License-Identifier: Apache-2.0
// Makapix Club: a pixel art social network. Its channels as a source, and
// (with a registration) this device as one of its players.
//
// Channels (a playset's "kind", and "identifier" where one is named):
//
//   promoted               the editors' picks. Plays without a registration
//   all                    every public artwork
//   user                   the posts of this player's owner
//   by_user   {sqid}       one artist's posts
//   hashtag   {tag}        a tag's posts
//   reactions {sqid}       what one person liked
//
// All but "promoted" need the device to be registered (the channel then
// says P3A_ERR_NO_KEY), because the server answers a player with what its
// owner may see. A channel is ordered as the site orders it: newest first,
// by when a post was listed (promoted, on that channel). A channel's
// offset is not honoured.
//
// An artwork's file comes from the vault, in the format it was uploaded
// in. It is pixel art and is scaled with sharp pixels.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot.
esp_err_t source_makapix_start(void);

// --- registration -------------------------------------------------------------------
//
// A device becomes a player when its owner types a code, shown on its
// screen, at makapix.club. Until that is done nothing is saved: a
// registration that is cancelled or whose code runs out leaves the device
// as it was, registered or not.

typedef enum {
    SOURCE_MAKAPIX_IDLE = 0,
    SOURCE_MAKAPIX_ASKING,        // asking the server for a code
    SOURCE_MAKAPIX_SHOWING_CODE,  // the code is on the screen; waiting for the owner
} source_makapix_phase_t;

#define SOURCE_MAKAPIX_RESULT_LEN 48

// Where this device stands with Makapix Club.
typedef struct {
    bool registered;      // it has a registration it can connect with
    char player_key[37];  // the player's id there; "" when not registered. With it anyone can ask
                          // the server for the player's token: never show or log it
    source_makapix_phase_t phase;
    char code[8];                 // while SHOWING_CODE
    uint32_t code_seconds_left;   // how long the code is still good for
    char last_result[SOURCE_MAKAPIX_RESULT_LEN];  // how the last registration ended: "registered", "cancelled",
                                                  // "the code ran out", "no code: <why>", "unregistered"; "" for none yet
} source_makapix_status_t;
void source_makapix_status(source_makapix_status_t *out);

// Starts a registration and returns at once: the status says how it goes.
// One already under way is ended and begun again with a new code.
esp_err_t source_makapix_register(void);
// Ends a registration under way. Nothing happens if there is none.
esp_err_t source_makapix_register_cancel(void);
// Forgets the registration. P3A_ERR_MISSING when there is none. The server
// is not told: the player stays on its owner's account until removed there.
esp_err_t source_makapix_unregister(void);

#ifdef __cplusplus
}
#endif
