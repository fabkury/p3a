// SPDX-License-Identifier: Apache-2.0
// Which artwork of the chosen channel: the random pick made less repetitive
// (change C) and the filter for very elongated artworks (change D). Pure: no
// IDF includes; host-tested (tests/host/player/test_sched_pick.c). Private to
// the scheduler.
//
// Repetition. The scheduler remembers the last SCHED_RECENT_MAX artworks
// shown, in memory only (a restart forgets). A random pick draws a
// candidate and accepts it with a chance that grows with how long ago it
// was shown: an artwork never shown is always accepted, one shown lately
// seldom, and none is ever excluded (the floor), except that the artwork
// shown last is not chosen again while the channel has another. After a few
// refused draws the candidate shown longest ago is taken. The result is a
// random order in which an artwork does not come back soon, at the cost of
// a few comparisons per pick and with no list of the channel in memory.
//
// Elongation. An artwork whose longer side is more than `max_tenths` / 10
// times its shorter side fills little of a square panel. Where the
// channel's source asks for the filter, such artworks are passed over.
// Sizes are known only once an artwork has been decoded, so one of unknown
// size is let through, measured by the player, and remembered.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sched_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_RECENT_MAX  256    // about two hours at the default 30 seconds each
#define SCHED_PICK_DRAWS  8      // candidates weighed before settling
#define SCHED_PICK_ATTEMPTS 32   // draws in all, those that land on a filtered artwork included
#define SCHED_PICK_FLOOR  4096   // of 65536: the least chance a candidate is given

typedef struct {
    uint32_t ids[SCHED_RECENT_MAX];
    uint32_t count, head;  // head: where the next goes
} sched_recent_t;

void sched_recent_clear(sched_recent_t *recent);

// An artwork was shown.
void sched_recent_add(sched_recent_t *recent, uint32_t id);

// How many artworks were shown since `id` was: 0 for the one shown last,
// SCHED_NONE for one not remembered.
uint32_t sched_recent_ago(const sched_recent_t *recent, uint32_t id);

// The chance, of 65536, with which a candidate shown `ago` artworks back is
// accepted in a channel of `available` artworks.
uint32_t sched_recent_chance(uint32_t ago, uint32_t available);

// True when the picture is more elongated than the limit. False when either
// side is unknown (0) or the limit is 0 (no filter). A ratio exactly at the
// limit passes.
bool sched_aspect_too_long(uint32_t width, uint32_t height, uint32_t max_tenths);

// What the pick needs to know about one artwork of the channel.
typedef struct {
    uint32_t id;             // its name in the list of what was shown
    uint32_t width, height;  // 0 when not yet measured
} sched_candidate_t;

// Describes the artwork at `ordinal` among the channel's playable ones.
// False when there is none there.
typedef bool (*sched_candidate_fn)(uint32_t ordinal, sched_candidate_t *out, void *ctx);

// Chooses an artwork of `channel`: its ordinal among the playable ones, or
// SCHED_NONE when the channel has none or every candidate drawn was too
// elongated. `max_tenths` is 0 for a channel that is not filtered.
uint32_t sched_pick_artwork(sched_core_t *core, uint32_t channel, sched_pick_mode_t mode,
                            const sched_recent_t *recent, uint32_t max_tenths, sched_candidate_fn describe, void *ctx);

#ifdef __cplusplus
}
#endif
