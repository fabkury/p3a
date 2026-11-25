// SPDX-License-Identifier: Apache-2.0
// The parts of the Makapix link that are decisions and text, not sockets.
// Pure: host-tested in tests/host/sources/test_makapix_link_logic.c.
// Private to the Makapix source.
//
// The link is MQTT over mutual TLS, and carries three things (D80):
// commands from the server, the player's presence, and reports of what it
// showed. Topics, with {k} the player's key:
//
//   makapix/player/{k}/command        in    {"command_id", "command_type", "payload", "timestamp"}
//   makapix/player/{k}/command/ack    out   {"command_id", "status": "ok" | "error" | "unsupported", "error"}
//   makapix/player/{k}/status         out   every 30 s; the server calls a player offline after
//                                           3 minutes of silence. Also the last will ("offline")
//   makapix/player/{k}/capabilities   out   retained, once per connection
//   makapix/player/{k}/state          out   retained, whole, whenever it changes (1.x sent one
//                                           field at a time, each replacing the retained whole)
//   makapix/player/{k}/view           out   one artwork seen; the server takes one per 5 s
//   makapix/player/{k}/view/ack       in    only when asked for; logged
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- reconnecting ---------------------------------------------------------------------
//
// After a failed or lost connection the next attempt waits: 15 s, doubling
// to 2 minutes, each wait drawn between half and one and a half times that
// (never over 2 minutes), so that players cut off together do not return
// together.
//
// A TLS handshake that fails says the broker and the player do not accept
// each other's certificates. Three in a row, or one where it is the
// broker's certificate the player rejects, call for one renewal of the
// certificates (which also brings a new CA). If that does not help, the
// registration is taken to be invalid and the link stops trying until a
// renewal succeeds or the device is registered again.

#define MAKAPIX_RETRY_FIRST_MS 15000
#define MAKAPIX_RETRY_MAX_MS   120000
#define MAKAPIX_HANDSHAKE_FAILURES 3

typedef struct {
    uint32_t base_ms;
    uint8_t handshake_failures;
    bool renewal_tried;  // in this outage
} makapix_retry_t;

typedef enum {
    MAKAPIX_RETRY_AGAIN = 0,  // wait and connect again
    MAKAPIX_RETRY_RENEW,      // renew the certificates first
    MAKAPIX_RETRY_INVALID,    // stop: the registration does not work
} makapix_retry_verdict_t;

void makapix_retry_reset(makapix_retry_t *r);  // at start, and on every connection made
// How long to wait before the next attempt; `random` is any 32 random bits.
uint32_t makapix_retry_delay(makapix_retry_t *r, uint32_t random);
// A connection attempt failed or a connection was lost.
makapix_retry_verdict_t makapix_retry_failed(makapix_retry_t *r, bool handshake_failed, bool broker_rejected);
// How the renewal went. `counted` false for an attempt that could not be
// made at all (no clock): it does not use up the outage's one renewal.
makapix_retry_verdict_t makapix_retry_renewed(makapix_retry_t *r, bool renewed, bool counted);

// --- renewing the certificates ---------------------------------------------------------
//
// A player's certificate lasts three years and the server renews one that
// has at most 200 days left. A player asks from 190 days before the end,
// each at its own moment within a week of that (so that players registered
// together do not all ask together); with two weeks left it asks at every
// check, and so does one whose certificate has run out. A renewal brings
// the CA's certificate too, which is how a player learns of a new CA.
//
// Nothing is asked while the device's clock is not set: every certificate
// would look expired, or none.

#define MAKAPIX_RENEW_WINDOW_S   (190u * 86400u)
#define MAKAPIX_RENEW_JITTER_S   (7u * 86400u)
#define MAKAPIX_RENEW_URGENT_S   (14u * 86400u)
#define MAKAPIX_CLOCK_PLAUSIBLE  1767225600u  // 2026-01-01: before the first 2.0 firmware

typedef enum {
    MAKAPIX_RENEW_NO_CLOCK = 0,  // cannot tell
    MAKAPIX_RENEW_NOT_DUE,
    MAKAPIX_RENEW_DUE,
} makapix_renew_due_t;

// `not_after` 0: the certificate's end could not be read; only a forced
// renewal goes ahead. `random` is any 32 random bits.
makapix_renew_due_t makapix_renew_due(uint32_t now_unix, uint32_t not_after, uint32_t random, bool forced);

// --- views -----------------------------------------------------------------------------
//
// A view is reported when a Makapix artwork has been on the panel, seen,
// for 5 seconds, and again every 30 seconds after that while it stays.
// Time during which it is covered (a screen over it, the panel paused)
// does not count.

#define MAKAPIX_VIEW_FIRST_MS  5000
#define MAKAPIX_VIEW_REPEAT_MS 30000

typedef struct {
    uint32_t post_id;   // 0: nothing of Makapix's is on the panel
    bool covered;
    int64_t seen_ms;    // how long it has been seen
    int64_t since_ms;   // when the count last moved; meaningless while covered
    int64_t next_ms;    // seen_ms at which the next report is due
} makapix_views_t;

void makapix_views_init(makapix_views_t *v);
// An artwork is on the panel (0: none of Makapix's). The same post again
// (the same artwork picked twice running) carries on counting.
void makapix_views_show(makapix_views_t *v, uint32_t post_id, int64_t now_ms);
void makapix_views_cover(makapix_views_t *v, bool covered, int64_t now_ms);
// True when a report is due; calling it takes the report as made.
bool makapix_views_due(makapix_views_t *v, int64_t now_ms);

// --- what is said ----------------------------------------------------------------------

#define MAKAPIX_TOPIC_LEN 96

typedef enum {
    MAKAPIX_TOPIC_COMMAND = 0,
    MAKAPIX_TOPIC_ACK,
    MAKAPIX_TOPIC_STATUS,
    MAKAPIX_TOPIC_CAPABILITIES,
    MAKAPIX_TOPIC_STATE,
    MAKAPIX_TOPIC_VIEW,
    MAKAPIX_TOPIC_VIEW_ACK,
    MAKAPIX_TOPIC_COUNT
} makapix_topic_t;
void makapix_topic(makapix_topic_t which, const char *player_key, char out[MAKAPIX_TOPIC_LEN]);

// "2026-11-25T20:15:00Z"; "1970-01-01T00:00:00Z" for 0, which the server
// reads as "the player's clock is not set".
void makapix_time_text(uint32_t unix_s, char out[24]);

// Each fills `out` with one JSON object. ESP_ERR_INVALID_SIZE when it does not fit.
esp_err_t makapix_msg_status(const char *player_key, bool online, uint32_t current_post_id, const char *version,
                             uint32_t now_unix, char *out, size_t len);
esp_err_t makapix_msg_capabilities(const char *version, char *out, size_t len);
esp_err_t makapix_msg_state(bool paused, int brightness, int rotation, char *out, size_t len);
// `status` "ok", "error" or "unsupported"; `error` may be NULL.
esp_err_t makapix_msg_ack(const char *command_id, const char *status, const char *error, char *out, size_t len);

typedef struct {
    uint32_t post_id;
    bool asked_for;       // a person asked for this artwork (shown as "artwork"); else it came up in a channel
    bool random;          // the order the channel is played in
    const char *channel;  // the server's name for the channel: "all", "by_user", ...; or "pinned", "sdcard", ...
    const char *user_sqid;  // for by_user and reactions; else NULL
    const char *hashtag;    // for hashtag; else NULL
    bool want_ack;
} makapix_view_t;
esp_err_t makapix_msg_view(const char *player_key, const makapix_view_t *view, uint32_t now_unix, char *out, size_t len);

// A command as it arrives. `payload` is a tree the caller deletes (never
// NULL on success: a command without one has an empty object).
typedef struct {
    char id[48];    // "" when the command has none
    char type[32];
    cJSON *payload;
} makapix_command_t;
// P3A_ERR_CORRUPT for anything that is not a command object with a type.
esp_err_t makapix_parse_command(const char *json, size_t len, makapix_command_t *out);

#ifdef __cplusplus
}
#endif
