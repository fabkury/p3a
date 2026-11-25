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

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot.
esp_err_t source_makapix_start(void);

// Where this device stands with Makapix Club.
typedef struct {
    bool registered;      // it has a registration it can connect with
    char player_key[37];  // the player's id there; "" when not registered. Not a secret by itself,
                          // but with it anyone can ask for the player's token: treat it as one
} source_makapix_status_t;
void source_makapix_status(source_makapix_status_t *out);

#ifdef __cplusplus
}
#endif
