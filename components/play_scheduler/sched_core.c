// SPDX-License-Identifier: Apache-2.0
#include "sched_core.h"

#include <limits.h>
#include <string.h>

uint32_t sched_prng_next(uint64_t *state)
{
    const uint64_t old = *state;
    *state = old * 6364136223846793005ULL + 1;
    const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    const uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

void sched_prng_seed(uint64_t *state, uint64_t seed)
{
    *state = seed;
    (void)sched_prng_next(state);  // once, to mix the seed in
}

// Shares SCHED_WSUM among the channels that have something to play, in
// proportion to their playset weights, or equally when none was given.
static void recalculate(sched_core_t *core)
{
    uint64_t total = 0;
    uint32_t offered = 0;
    for (uint32_t i = 0; i < core->count; i++) {
        if (core->channels[i].available > 0) {
            total += core->channels[i].spec_weight;
            offered++;
        }
    }
    for (uint32_t i = 0; i < core->count; i++) {
        sched_channel_t *ch = &core->channels[i];
        if (ch->available == 0) {
            ch->weight = 0;
        } else if (total == 0) {
            ch->weight = SCHED_WSUM / offered;
        } else {
            ch->weight = (uint32_t)((uint64_t)ch->spec_weight * SCHED_WSUM / total);
        }
    }
}

void sched_core_init(sched_core_t *core, uint32_t count, uint64_t seed)
{
    memset(core, 0, sizeof(*core));
    core->count = count > SCHED_MAX_CHANNELS ? SCHED_MAX_CHANNELS : count;
    core->last_selected = -1;
    sched_prng_seed(&core->rng, seed);
    for (uint32_t i = 0; i < core->count; i++) {
        // Each channel its own sequence, so that what one channel plays does
        // not depend on how often the others were chosen.
        sched_prng_seed(&core->channels[i].rng, seed ^ (0x9E3779B97F4A7C15ULL * (i + 1)));
    }
}

void sched_core_set_weight(sched_core_t *core, uint32_t channel, uint32_t spec_weight)
{
    if (channel < core->count) {
        core->channels[channel].spec_weight = spec_weight;
        recalculate(core);
    }
}

void sched_core_set_available(sched_core_t *core, uint32_t channel, uint32_t available)
{
    if (channel >= core->count) {
        return;
    }
    sched_channel_t *ch = &core->channels[channel];
    const bool flipped = (ch->available == 0) != (available == 0);
    ch->available = available;
    if (flipped) {
        // A channel that has just got its first artwork starts level with
        // the others, not with whatever it was owed when it ran dry.
        ch->credit = 0;
        recalculate(core);
    }
}

static uint32_t select_swrr(sched_core_t *core)
{
    uint32_t candidates[SCHED_MAX_CHANNELS];
    uint32_t candidate_count = 0;
    int32_t best = INT32_MIN;
    for (uint32_t i = 0; i < core->count; i++) {
        const sched_channel_t *ch = &core->channels[i];
        if (ch->weight == 0) {
            continue;
        }
        if (ch->credit > best) {
            best = ch->credit;
            candidate_count = 0;
        }
        if (ch->credit == best) {
            candidates[candidate_count++] = i;
        }
    }
    if (candidate_count == 0) {
        return SCHED_NONE;
    }
    // Ties are broken at random, so that equal channels do not always play
    // in playset order.
    return candidates[candidate_count == 1 ? 0 : sched_prng_next(&core->rng) % candidate_count];
}

// P(i) is proportional to weight_i * clamp(1 + 0.8 * credit_i / WSUM, 0.1, 3):
// random, but a channel that has fallen behind its share is more likely and
// one that is ahead less so, which keeps long runs from drifting.
static uint32_t select_stochastic(sched_core_t *core)
{
    float chance[SCHED_MAX_CHANNELS];
    float sum = 0.0f;
    uint32_t last = SCHED_NONE;
    for (uint32_t i = 0; i < core->count; i++) {
        const sched_channel_t *ch = &core->channels[i];
        chance[i] = 0.0f;
        if (ch->weight == 0) {
            continue;
        }
        float factor = 1.0f + 0.8f * (float)ch->credit / (float)SCHED_WSUM;
        factor = factor < 0.1f ? 0.1f : factor > 3.0f ? 3.0f : factor;
        chance[i] = (float)ch->weight * factor;
        sum += chance[i];
        last = i;
    }
    if (last == SCHED_NONE) {
        return SCHED_NONE;
    }
    const float r = (float)sched_prng_next(&core->rng) / 4294967296.0f * sum;
    float running = 0.0f;
    for (uint32_t i = 0; i < core->count; i++) {
        if (chance[i] > 0.0f) {
            running += chance[i];
            if (r < running) {
                return i;
            }
        }
    }
    return last;  // rounding left r at the very top
}

uint32_t sched_core_select(sched_core_t *core, sched_select_mode_t mode)
{
    core->last_selected = -1;
    for (uint32_t i = 0; i < core->count; i++) {
        core->channels[i].credit += (int32_t)core->channels[i].weight;
    }
    const uint32_t chosen = mode == SCHED_SELECT_STOCHASTIC ? select_stochastic(core) : select_swrr(core);
    if (chosen == SCHED_NONE) {
        return SCHED_NONE;  // every weight is 0, so nothing was added either
    }
    core->channels[chosen].credit -= SCHED_WSUM;
    core->last_selected = (int32_t)chosen;
    return chosen;
}

void sched_core_undo_select(sched_core_t *core)
{
    if (core->last_selected < 0) {
        return;
    }
    core->channels[core->last_selected].credit += SCHED_WSUM;
    for (uint32_t i = 0; i < core->count; i++) {
        core->channels[i].credit -= (int32_t)core->channels[i].weight;
    }
    core->last_selected = -1;
}

uint32_t sched_core_pick(sched_core_t *core, uint32_t channel, sched_pick_mode_t mode)
{
    if (channel >= core->count || core->channels[channel].available == 0) {
        return SCHED_NONE;
    }
    sched_channel_t *ch = &core->channels[channel];
    if (mode == SCHED_PICK_RECENCY) {
        if (ch->cursor >= ch->available) {
            ch->cursor = 0;  // past the end, or the channel shrank: start again at the newest
        }
        return ch->cursor++;
    }
    return sched_prng_next(&ch->rng) % ch->available;
}
