// SPDX-License-Identifier: Apache-2.0
// Makapix Club's answers read into records, and the addresses and
// questions p3a puts to it. Pure (cJSON and the record only): host-tested
// on responses recorded from the service
// (tests/host/sources/test_makapix_parse.c). Private to the Makapix source.
//
// How Makapix is asked (D80):
//   - A registered player asks over HTTPS, with its bearer token:
//     POST /api/player/rpc, body {"request_type":"query_posts", ...}. The
//     answer is {"success", "posts", "has_more", "next_cursor"}. The
//     server filters by what the player's owner may see.
//   - A player with no registration can list one channel, "promoted", from
//     the public feed: GET /api/v1/feed/promoted, answer {"items",
//     "next_cursor"}.
//   The two name a post's number differently ("post_id", "id") and are
//   otherwise read by the same code.
//
// What identifies an artwork:
//   - the post's number: the record's key, what views and reactions name;
//   - its storage key, a UUID: the record's identity, and where the file
//     is. A post whose picture is replaced gets a new storage key, so to
//     p3a it is a new artwork, and a file's bytes never change.
//
// Where the file is: https://{vault}/{aa}/{bb}/{storage key}.{format},
// with aa and bb the first two bytes of SHA-256 of the key's text, each
// masked to six bits, in hex. The format is the post's native one, taken
// from the end of its "art_url" (else from "native_format").
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAKAPIX_HOST        "makapix.club"
#define MAKAPIX_VAULT_HOST  "vault.makapix.club"
#define MAKAPIX_PAGE_POSTS  50   // the most query_posts gives at once
#define MAKAPIX_CURSOR_LEN  256  // a cursor, with its terminator
#define MAKAPIX_UUID_TEXT   37   // a UUID as text, with its terminator

// --- small things -------------------------------------------------------------------

// "f14ebf48-c4a3-449f-92c1-58a83224d984" to 16 bytes and back (lower
// case). False for anything that is not exactly a hyphenated UUID.
bool makapix_uuid_parse(const char *text, uint8_t out[16]);
void makapix_uuid_text(const uint8_t uuid[16], char out[MAKAPIX_UUID_TEXT]);

// An ISO 8601 time in UTC ("2026-11-24T16:06:18.461787Z", "...+00:00",
// with or without the fraction) as Unix seconds; 0 for anything else,
// another offset included, and for a time before 2000.
uint32_t makapix_time(const char *text);

// --- channels -----------------------------------------------------------------------

typedef enum {
    MAKAPIX_CHANNEL_ALL = 0,     // kind "all": every public artwork, newest listing first
    MAKAPIX_CHANNEL_PROMOTED,    // "promoted": the editors' picks, newest promotion first
    MAKAPIX_CHANNEL_USER,        // "user": the posts of this player's owner
    MAKAPIX_CHANNEL_BY_USER,     // "by_user": one artist's posts; identifier: the artist's sqid
    MAKAPIX_CHANNEL_HASHTAG,     // "hashtag": identifier: the tag, without '#'
    MAKAPIX_CHANNEL_REACTIONS,   // "reactions": what one person liked; identifier: their sqid
    MAKAPIX_CHANNEL_COUNT
} makapix_channel_t;

// Reads a channel's kind and checks its identifier: needed, and only
// letters, digits, '_' and '-' (a sqid, a tag), for the three kinds that
// take one. False for anything else.
bool makapix_channel_parse(const char *kind, const char *identifier, makapix_channel_t *out);
// The name the server knows the channel by ("all", "by_user", ...).
const char *makapix_channel_name(makapix_channel_t channel);

// --- asking -------------------------------------------------------------------------

// The body of one query_posts request. `cursor` NULL or "" for the first
// page. ESP_ERR_INVALID_SIZE when it does not fit.
esp_err_t makapix_query_body(makapix_channel_t channel, const char *identifier, const char *cursor, char *out,
                             size_t len);

// The address of one page of the public promoted feed.
esp_err_t makapix_feed_url(const char *cursor, char *out, size_t len);

// The body of a reaction (the thumbs-up, the one p3a gives) or its taking back.
esp_err_t makapix_reaction_body(uint32_t post_id, bool revoke, char *out, size_t len);

// --- reading ------------------------------------------------------------------------

typedef struct {
    uint32_t count;                    // records made
    uint32_t items;                    // posts the page held (some are passed over: playlists, posts with no usable file)
    bool has_more;
    char cursor[MAKAPIX_CURSOR_LEN];   // for the next page; "" when there is none
} makapix_page_t;

// Reads one page, of either kind of answer, into `out` (room for `room`
// records; a page holds at most MAKAPIX_PAGE_POSTS that p3a asked for, but
// the count is the answer's, so give room and it is never overrun).
// `promoted` says which date orders the channel.
//   P3A_ERR_CORRUPT       not JSON, or not an answer of that shape
//   P3A_ERR_MISSING       the server says there is no such user or tag
//   P3A_ERR_AUTH          it says the player is not (or no longer) registered
//   ESP_FAIL              it refused for another reason
esp_err_t makapix_parse_page(const char *json, bool promoted, p3a_record_t *out, uint32_t room, makapix_page_t *page);

// Reads the answer to a reaction: ESP_OK when the server took it.
esp_err_t makapix_parse_result(const char *json);

// Reads {"api_token": "..."} (the answer to a token rotation). P3A_ERR_CORRUPT
// for an answer without one, or one that does not fit `len`. The token is a
// secret: never log it.
esp_err_t makapix_parse_token(const char *json, char *out, size_t len);

// --- registration -------------------------------------------------------------------
//
// A device becomes a player in two steps. It asks for a code
// (POST /api/player/provision, answered 201) and shows it; its owner types
// the code at makapix.club. Meanwhile the device asks for its credentials
// (GET /api/player/{player_key}/credentials), answered 404 until the owner
// has done so, then once with everything: the three PEMs, the broker, and a
// token (only on the first asking).

#define MAKAPIX_CODE_LEN 8  // six characters today; with the terminator

typedef struct {
    char player_key[MAKAPIX_UUID_TEXT];
    char code[MAKAPIX_CODE_LEN];
    uint32_t expires_at;  // Unix seconds; 0 when the answer's time could not be read
    char host[64];        // the broker
    uint16_t port;
} makapix_provision_t;

// P3A_ERR_CORRUPT unless the answer has a player key that is a UUID, a code
// of 4 to 7 letters and digits, and a broker.
esp_err_t makapix_parse_provision(const char *json, makapix_provision_t *out);

typedef struct {
    // Each points into the caller's `pems` buffer (below), terminated.
    const char *ca, *cert, *key;
    char host[64];  // "" and 0 when the answer names no broker: the one from the provision stands
    uint16_t port;
    char token[128];  // "" when the answer has none
} makapix_credentials_t;

// Reads the credentials. The three PEMs are copied into `pems` (room for
// `pems_len` bytes), one after the other, and `out` points at them: the
// JSON can then be wiped. P3A_ERR_CORRUPT unless all three are there, each
// looks like a PEM ("-----BEGIN ") and is shorter than 4096 bytes.
esp_err_t makapix_parse_credentials(const char *json, char *pems, size_t pems_len, makapix_credentials_t *out);

// --- the file -----------------------------------------------------------------------

// Where a record's file is. `digest` is SHA-256 of the storage key's text
// (makapix_uuid_text), which the caller computes. P3A_ERR_MISSING for a
// record whose format is not known (it was listed without one).
esp_err_t makapix_vault_url(const p3a_record_t *record, const uint8_t digest[32], char *out, size_t len);

#ifdef __cplusplus
}
#endif
