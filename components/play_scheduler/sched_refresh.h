// SPDX-License-Identifier: Apache-2.0
// When a channel is listed again, and how long to wait after a listing that
// failed. Pure: no IDF includes, the caller passes the time; host-tested
// (tests/host/player/test_sched_refresh.c). Private to the scheduler.
//
// One of these per channel. It answers one question, "is this channel due
// now?", and is told how each listing ended:
//
//   listed          wait the source's interval (never again, for a source
//                   that is listed only when something says it changed)
//   rate limited    wait what the server asked for, at least 15 minutes
//   no key, or the  wait an hour, or until someone says the settings
//   key is refused  changed: asking again sooner cannot help
//   the card        stop: the card has failed until the restart
//   anything else   wait one minute, then two, four, up to an hour
//
// A channel that needs the network is never due while there is none. When
// the network returns, the caller asks again for the channels whose last
// listing failed, so that they do not sit out a wait that was the network's
// fault.
//
// 1.x had this as flags and timestamps in five places per source, and a
// state that could be both "refreshing" and "waiting".
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_REFRESH_RATE_LIMIT_MIN_S 900
#define SCHED_REFRESH_KEY_WAIT_S       3600
#define SCHED_REFRESH_RETRY_MIN_S      60
#define SCHED_REFRESH_RETRY_MAX_S      3600

typedef enum {
    SCHED_REFRESH_IDLE = 0,  // listed, or waiting for its time
    SCHED_REFRESH_RUNNING,
    SCHED_REFRESH_STOPPED,   // not listed again before a restart
} sched_refresh_state_t;

// How a listing ended.
typedef enum {
    SCHED_LISTED = 0,
    SCHED_LIST_RATE_LIMITED,
    SCHED_LIST_NO_KEY,    // no key, or the key was refused
    SCHED_LIST_CARD,      // the card has failed
    SCHED_LIST_FAILED,    // anything else: the network, the server, memory
} sched_list_result_t;

typedef struct {
    uint8_t state;         // sched_refresh_state_t
    uint8_t last;          // sched_list_result_t of the last listing
    bool needs_network;
    bool ever_listed;      // a listing has succeeded, now or in an earlier run (its index was on the card)
    uint32_t interval_s;   // 0: only when asked
    uint32_t failures;     // in a row
    int64_t due_ms;        // when it is next due; INT64_MAX for "not by itself"
} sched_refresh_t;

// --- which comes first, a listing or a download ---------------------------------------
//
// Listing comes before downloading: a playset's channels are all known
// before the card fills with the first one's artworks. But a playset with
// nothing to show should show something as soon as it can, not after its
// last channel is listed (with sixty channels new to the device, a minute
// or more). So while the playset has fewer playable artworks than this, a
// download goes first whenever there is one to make.
#define SCHED_START_FLOOR 3

// `playable`: artworks of the active playset that could be shown now.
static inline bool sched_refresh_download_first(uint32_t playable)
{
    return playable < SCHED_START_FLOOR;
}

// A full listing of a large channel is many pages (Makapix: 2,048 posts,
// some twenty seconds). While the playset has next to nothing to show, a
// channel the device knows nothing of is first listed short, its newest
// few, and listed in full straight after: once per channel, and only for a
// source asked over the network.
#define SCHED_QUICK_RECORDS 32

static inline bool sched_refresh_quick_first(uint32_t playable, bool remote, bool knows_any, bool quick_done)
{
    return remote && !knows_any && !quick_done && playable < SCHED_START_FLOOR;
}

// A channel that was never listed: due at once.
void sched_refresh_init(sched_refresh_t *r, uint32_t interval_s, bool needs_network);

// The channel's index was found on the card, written `age_s` seconds ago
// (pass a negative age when that is not known: the clock is not set). It is
// due when the interval since then has passed; with an unknown age, at once.
void sched_refresh_known(sched_refresh_t *r, int64_t age_s, int64_t now_ms);

// Something says the channel changed, or the settings it depends on did:
// due now, whatever it was waiting for. Not while it is being listed.
void sched_refresh_ask(sched_refresh_t *r, int64_t now_ms);

bool sched_refresh_due(const sched_refresh_t *r, int64_t now_ms, bool network_up);

void sched_refresh_started(sched_refresh_t *r);

// `retry_after_s` is what the server asked for with a rate limit; 0 if nothing.
void sched_refresh_finished(sched_refresh_t *r, sched_list_result_t result, uint32_t retry_after_s, int64_t now_ms);

// Seconds until it is due by itself; 0 when due, -1 when it will not be.
int64_t sched_refresh_wait_s(const sched_refresh_t *r, int64_t now_ms);

#ifdef __cplusplus
}
#endif
