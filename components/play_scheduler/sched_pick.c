// SPDX-License-Identifier: Apache-2.0
// Which artwork of the chosen channel. See sched_pick.h. Pure.

#include "sched_pick.h"

#include <string.h>

void sched_recent_clear(sched_recent_t *recent)
{
    memset(recent, 0, sizeof(*recent));
}

void sched_recent_add(sched_recent_t *recent, uint32_t id)
{
    recent->ids[recent->head] = id;
    recent->head = (recent->head + 1) % SCHED_RECENT_MAX;
    if (recent->count < SCHED_RECENT_MAX) {
        recent->count++;
    }
}

uint32_t sched_recent_ago(const sched_recent_t *recent, uint32_t id)
{
    for (uint32_t ago = 0; ago < recent->count; ago++) {
        const uint32_t at = (recent->head + SCHED_RECENT_MAX - 1 - ago) % SCHED_RECENT_MAX;
        if (recent->ids[at] == id) {
            return ago;
        }
    }
    return SCHED_NONE;
}

uint32_t sched_recent_chance(uint32_t ago, uint32_t available)
{
    // In a small channel everything was shown lately; what counts there is
    // how long ago compared with the channel's size.
    uint32_t window = available < SCHED_RECENT_MAX ? available : SCHED_RECENT_MAX;
    if (window < 2) {
        window = 2;
    }
    if (ago == SCHED_NONE || ago >= window) {
        return 65536;
    }
    // Rising with the square of the time: steep enough that "lately" is
    // rare, while the far half of the window is nearly as likely as new.
    return SCHED_PICK_FLOOR + (uint32_t)(((uint64_t)(65536 - SCHED_PICK_FLOOR) * ago * ago) / ((uint64_t)window * window));
}

bool sched_aspect_too_long(uint32_t width, uint32_t height, uint32_t max_tenths)
{
    if (width == 0 || height == 0 || max_tenths == 0) {
        return false;
    }
    const uint64_t longer = width > height ? width : height;
    const uint64_t shorter = width > height ? height : width;
    return longer * 10 > (uint64_t)max_tenths * shorter;
}

uint32_t sched_pick_artwork(sched_core_t *core, uint32_t channel, sched_pick_mode_t mode,
                            const sched_recent_t *recent, uint32_t max_tenths, sched_candidate_fn describe, void *ctx)
{
    if (channel >= core->count || core->channels[channel].available == 0) {
        return SCHED_NONE;
    }
    sched_channel_t *ch = &core->channels[channel];
    const uint32_t available = ch->available;
    // Draws that land on a filtered artwork are not counted as candidates,
    // within a bound: a channel that is mostly elongated still yields its
    // few good ones, and one that is all elongated ends the search.
    const uint32_t attempts = mode == SCHED_PICK_RECENCY && available < SCHED_PICK_ATTEMPTS ? available : SCHED_PICK_ATTEMPTS;
    uint32_t oldest = SCHED_NONE, oldest_ago = 0;
    for (uint32_t i = 0, drawn = 0; i < attempts && drawn < SCHED_PICK_DRAWS; i++) {
        const uint32_t ordinal = sched_core_pick(core, channel, mode);
        sched_candidate_t candidate = {0};
        if (ordinal == SCHED_NONE || !describe(ordinal, &candidate, ctx)) {
            continue;
        }
        if (sched_aspect_too_long(candidate.width, candidate.height, max_tenths)) {
            continue;
        }
        drawn++;
        if (mode == SCHED_PICK_RECENCY) {
            return ordinal;  // the channel's own order, minus what the filter refuses
        }
        const uint32_t ago = sched_recent_ago(recent, candidate.id);
        if (ago == SCHED_NONE) {
            return ordinal;
        }
        if (oldest == SCHED_NONE || ago > oldest_ago) {
            oldest = ordinal;
            oldest_ago = ago;
        }
        if (ago == 0 && available > 1) {
            continue;  // never the same artwork twice in a row while there is another
        }
        if ((sched_prng_next(&ch->rng) & 0xFFFF) < sched_recent_chance(ago, available)) {
            return ordinal;
        }
    }
    if (oldest != SCHED_NONE && oldest_ago == 0 && available > 1) {
        // Every draw landed on the artwork shown last. Its neighbour, then.
        const uint32_t other = (oldest + 1) % available;
        sched_candidate_t candidate = {0};
        if (describe(other, &candidate, ctx) &&
            !sched_aspect_too_long(candidate.width, candidate.height, max_tenths)) {
            return other;
        }
    }
    return oldest;
}
