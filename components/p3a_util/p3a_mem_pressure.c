// SPDX-License-Identifier: Apache-2.0
// The pure half of p3a_mem.h: thresholds and the level decision.
#include "p3a_mem.h"

p3a_mem_thresholds_t p3a_mem_default_thresholds(void)
{
    return (p3a_mem_thresholds_t){
        .low_free_bytes = 64 * 1024,
        .low_largest_block = 24 * 1024,
        .critical_free_bytes = 40 * 1024,
        .critical_largest_block = 12 * 1024,
        .recover_margin_percent = 25,
    };
}

// 64-bit: a threshold plus its margin may not fit in 32 bits.
static uint64_t with_margin(uint32_t value, uint8_t percent)
{
    return ((uint64_t)value * (100u + percent)) / 100u;
}

// Is the sample below the given pair of thresholds?
static bool below(uint32_t free_bytes, uint32_t largest_block, uint64_t free_limit, uint64_t block_limit)
{
    return free_bytes < free_limit || largest_block < block_limit;
}

p3a_mem_level_t p3a_mem_evaluate(p3a_mem_level_t previous, uint32_t free_bytes, uint32_t largest_block,
                                 const p3a_mem_thresholds_t *t)
{
    // What the sample says with no memory of the past.
    p3a_mem_level_t raw = P3A_MEM_OK;
    if (below(free_bytes, largest_block, t->critical_free_bytes, t->critical_largest_block)) {
        raw = P3A_MEM_CRITICAL;
    } else if (below(free_bytes, largest_block, t->low_free_bytes, t->low_largest_block)) {
        raw = P3A_MEM_LOW;
    }
    if (raw >= previous) {
        return raw;  // same or worse: immediately
    }

    // Better than before: only if it clears the level it is leaving by the margin.
    const uint8_t m = t->recover_margin_percent;
    if (previous == P3A_MEM_CRITICAL &&
        below(free_bytes, largest_block, with_margin(t->critical_free_bytes, m),
              with_margin(t->critical_largest_block, m))) {
        return P3A_MEM_CRITICAL;
    }
    if (below(free_bytes, largest_block, with_margin(t->low_free_bytes, m), with_margin(t->low_largest_block, m))) {
        // Out of CRITICAL (or still in LOW), but not clear of LOW.
        return P3A_MEM_LOW;
    }
    return P3A_MEM_OK;
}

const char *p3a_mem_level_name(p3a_mem_level_t level)
{
    switch (level) {
    case P3A_MEM_OK:
        return "ok";
    case P3A_MEM_LOW:
        return "low";
    case P3A_MEM_CRITICAL:
        return "critical";
    }
    return "invalid";
}
