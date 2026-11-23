// SPDX-License-Identifier: Apache-2.0
// The rules of a turn: when the device moves on by itself, and what a
// failure leads to. Pure: no IDF includes, no clock of its own (the caller
// passes the time in milliseconds); host-tested
// (tests/host/player/test_sched_turn.c). Private to the scheduler.
//
// A turn starts when something asks for an artwork and ends when one is on
// the panel or the attempt is given up. Who asked decides how a failure is
// treated:
//
//   SCHED_TURN_AUTO   the device itself (the dwell timer, the first artwork
//                     after boot). Another artwork is tried, a few times;
//                     then it waits for the next dwell. Never reported.
//   SCHED_TURN_ANY    a person asked for "next". Another artwork is tried,
//                     the same few times; then the failure is reported.
//   SCHED_TURN_THIS   a person asked for one artwork (back, forward, "show
//                     this"). Nothing else is tried; the failure is reported.
//
// 1.x retried only automatic swaps and showed an error for every failed
// manual one, though a person who presses "next" wants the next artwork that
// plays, not a message about one that does not.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_TURN_RETRIES       3      // other artworks tried after a failure, per turn
#define SCHED_TURN_SKIPS         6      // artworks passed over by a filter after loading, per turn
#define SCHED_TURN_DELETE_GAP_MS 60000  // at most one damaged blob is deleted per minute

typedef enum {
    SCHED_TURN_AUTO = 0,
    SCHED_TURN_ANY,
    SCHED_TURN_THIS,
} sched_turn_origin_t;

// What went wrong with the request, as far as the rules care.
typedef enum {
    SCHED_FAIL_MISSING = 0,   // the file is not there
    SCHED_FAIL_CORRUPT,       // the file is damaged
    SCHED_FAIL_NOT_PLAYABLE,  // a sound file this device cannot play
    SCHED_FAIL_OTHER,         // memory, a card that cannot be read: nothing is wrong with the artwork
} sched_fail_t;

typedef struct {
    bool retry;        // ask for another artwork now, under the same turn
    bool report;       // tell the person: the turn ended with nothing shown
    bool set_aside;    // do not offer this artwork again until its channel is listed anew
    bool delete_blob;  // its bytes are damaged: delete them (the caller never does for a user's own file)
} sched_turn_verdict_t;

typedef struct {
    uint32_t dwell_s;      // 0: never moves on by itself
    bool paused;
    uint32_t holds;        // screens that are up over the artwork
    bool busy;             // a turn is under way
    uint8_t origin;        // of that turn
    uint8_t retries, skips;
    bool timing;           // an artwork is on the panel and its time is being counted
    bool idle;             // nothing is on the panel and the last attempt to put something there failed
    int64_t since_ms;      // when the count started, or the failed attempt ended
    bool deleted_any;
    int64_t deleted_ms;    // when a blob was last deleted
} sched_turn_t;

void sched_turn_init(sched_turn_t *turn, uint32_t dwell_s);

// A request went to the player on behalf of `origin`. Replaces any turn
// under way.
void sched_turn_begin(sched_turn_t *turn, sched_turn_origin_t origin);

// The artwork is on the panel: the turn is over and its time starts.
void sched_turn_shown(sched_turn_t *turn, int64_t now_ms);

// The request failed. Says what to do; when it says `retry` the turn goes on
// and the caller asks for another artwork.
sched_turn_verdict_t sched_turn_failed(sched_turn_t *turn, sched_fail_t fail, int64_t now_ms);

// The artwork loaded but a filter rejects it. True: pass it over and ask for
// another under the same turn. False: enough were passed over; the turn is
// given up (call sched_turn_gave_up()).
bool sched_turn_skip(sched_turn_t *turn);

// The turn ends with nothing new on the panel and nothing to try (no
// channel had an artwork). The next attempt is a dwell from now, also when
// the panel is empty: a device whose first picks all failed tries again
// instead of staying black.
void sched_turn_gave_up(sched_turn_t *turn, int64_t now_ms);

// Something that is not the scheduler's is on the panel now (a test playing
// a file directly). Nothing is timed until the scheduler shows something.
void sched_turn_stopped(sched_turn_t *turn);

// Whether it is time to move on by itself. `playable` is how many artworks
// could be chosen; with one or none there is nothing to move on to (with
// an empty panel, one is enough).
bool sched_turn_due(const sched_turn_t *turn, int64_t now_ms, uint32_t playable);

// Seconds until sched_turn_due() could say yes, or -1 when it will not
// (paused, held, no dwell, nothing on the panel).
int32_t sched_turn_remaining_s(const sched_turn_t *turn, int64_t now_ms);

// The setting changed. The artwork on the panel keeps the time it has had.
void sched_turn_set_dwell(sched_turn_t *turn, uint32_t dwell_s);

// Pause, and screens over the artwork: time does not count meanwhile, and
// the artwork gets a full dwell afterwards.
void sched_turn_set_paused(sched_turn_t *turn, bool paused, int64_t now_ms);
void sched_turn_hold(sched_turn_t *turn);
void sched_turn_release(sched_turn_t *turn, int64_t now_ms);

#ifdef __cplusplus
}
#endif
