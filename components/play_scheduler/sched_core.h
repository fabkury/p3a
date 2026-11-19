// SPDX-License-Identifier: Apache-2.0
// The scheduler's arithmetic: which channel plays next, and which of its
// artworks. Pure: no IDF includes, no indices, no files; host-tested
// (tests/host/player/test_sched_core.c).
//
// The core knows a channel only as a weight and a count of artworks that can
// be played now. It answers with a channel number and an ordinal "the n-th
// playable artwork of that channel", which its caller turns into a record
// with p3a_index_nth_available(). Private to the scheduler.
//
// 1.x had three recency picks and three random picks, one per record layout.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_MAX_CHANNELS 64
#define SCHED_WSUM         65536  // weights are normalised to this sum
#define SCHED_NONE         UINT32_MAX

// How the next channel is chosen (the `channel_select` setting).
typedef enum {
    SCHED_SELECT_SWRR = 0,    // smooth weighted round robin: exact proportions, even spacing
    SCHED_SELECT_STOCHASTIC,  // weighted random, nudged by how far behind each channel is
} sched_select_mode_t;

// How an artwork is chosen within the channel (the `pick_mode` setting).
typedef enum {
    SCHED_PICK_RANDOM = 0,
    SCHED_PICK_RECENCY,  // in the channel's own order, newest first, round and round
} sched_pick_mode_t;

typedef struct {
    uint32_t spec_weight;  // from the playset; 0 means "no preference"
    uint32_t available;    // artworks playable now; a channel with none is never chosen
    uint32_t weight;       // normalised share of SCHED_WSUM; 0 when nothing is available
    int32_t credit;
    uint32_t cursor;       // recency: the ordinal of the next pick
    uint64_t rng;          // random: this channel's own sequence
} sched_channel_t;

typedef struct {
    sched_channel_t channels[SCHED_MAX_CHANNELS];
    uint32_t count;
    uint64_t rng;          // for choosing channels
    int32_t last_selected; // for sched_core_undo_select(); -1 when there is nothing to undo
} sched_core_t;

// Starts over with `count` channels (at most SCHED_MAX_CHANNELS), no weights,
// nothing available. The same seed gives the same sequence of choices.
void sched_core_init(sched_core_t *core, uint32_t count, uint64_t seed);

// What the playset says about a channel. Weights are relative to each other;
// if every channel's is 0 they share equally.
void sched_core_set_weight(sched_core_t *core, uint32_t channel, uint32_t spec_weight);

// How many of a channel's artworks can be played now. Call whenever it
// changes (a download landed, an eviction, a refresh).
void sched_core_set_available(sched_core_t *core, uint32_t channel, uint32_t available);

// Chooses the next channel, or SCHED_NONE when no channel has anything to
// play. The chosen channel pays for its turn.
uint32_t sched_core_select(sched_core_t *core, sched_select_mode_t mode);

// Takes back the last sched_core_select() as if it had not happened: for a
// turn that ended with nothing shown (the artwork would not load). The
// channel's cursor and random sequence are not rewound, so the next pick is
// a different artwork. A second call without a select in between does
// nothing.
void sched_core_undo_select(sched_core_t *core);

// Chooses an artwork of `channel`: its ordinal among the channel's playable
// artworks (0 is the first in channel order), or SCHED_NONE when it has none.
uint32_t sched_core_pick(sched_core_t *core, uint32_t channel, sched_pick_mode_t mode);

// The generator behind every random choice (PCG32). Exposed for tests and
// for seeding.
uint32_t sched_prng_next(uint64_t *state);
void sched_prng_seed(uint64_t *state, uint64_t seed);

#ifdef __cplusplus
}
#endif
