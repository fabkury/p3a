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

#include "cJSON.h"
#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot.
esp_err_t source_makapix_start(void);

// A record for one Makapix artwork, from what a message about it says
// (a command to show it): the post's number, its storage key, and its
// format, as the end of its address (`art_url`, may be NULL) or by name
// (`native_format`, may be NULL: "webp", "gif", "png", "bmp").
// ESP_ERR_INVALID_ARG unless all three can be made out. Pure.
esp_err_t source_makapix_record(uint32_t post_id, const char *storage_key, const char *art_url,
                                const char *native_format, p3a_record_t *out);

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
#define SOURCE_MAKAPIX_RENEWAL_LEN 24

// The link: the connection over which the server sends commands and the
// player says that it is there and what it shows.
typedef enum {
    SOURCE_MAKAPIX_LINK_NONE = 0,    // not registered
    SOURCE_MAKAPIX_LINK_WAITING,     // registered; no network, or between attempts
    SOURCE_MAKAPIX_LINK_CONNECTING,
    SOURCE_MAKAPIX_LINK_CONNECTED,
    SOURCE_MAKAPIX_LINK_INVALID,     // the server does not accept this player's certificates, and a
                                     // renewal did not help: register again
} source_makapix_link_t;

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
    source_makapix_link_t link;
    uint32_t seen_post_id;        // the Makapix post seen on the panel now; 0 for none
    uint32_t cert_not_after;      // when the player's certificate ends, Unix seconds; 0 if not known yet
    char renewal[SOURCE_MAKAPIX_RENEWAL_LEN];     // how the last look at renewing it went: "renewed", "not_due",
                                                  // "no_clock", "auth_failed", "player_gone", "rate_limited", "error"
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

// --- commands -----------------------------------------------------------------------
//
// The server can tell a player what to do (the owner's buttons on the
// site). The source hands each command, as it came, to one handler, which a
// layer that may call the scheduler registers (p3a_net). The handler runs
// on the Makapix task, never on the connection's own.

typedef void (*source_makapix_command_fn)(const char *type, const cJSON *payload, const char *command_id, void *ctx);
void source_makapix_set_command_handler(source_makapix_command_fn handler, void *ctx);

// Answers a command that waits for an answer: `status` "ok", "error" or
// "unsupported", `error` the reason or NULL. Nothing is sent for a command
// that came without an id.
void source_makapix_ack(const char *command_id, const char *status, const char *error);

// Hands the source a command as the link would (the bytes of one message
// on the command topic). For the test hooks, which cannot be the server.
// ESP_ERR_NO_MEM when it cannot be queued.
esp_err_t source_makapix_deliver(const char *json, size_t len);

// Whether the player is paused: part of the state the server shows the owner.
void source_makapix_set_paused(bool paused);

#ifdef __cplusplus
}
#endif
