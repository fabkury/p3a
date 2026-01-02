// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_nae.c
 * @brief New Artwork Events (NAE) pool management
 *
 * NAE provides temporary, probabilistic, out-of-band exposure for newly
 * published artworks. The pool has a maximum size (J=32) and uses priority
 * decay to ensure fair rotation.
 *
 * Key behaviors:
 * - New entries start with 50% priority
 * - Duplicate entries have priority reset to 50%
 * - Priority halves on each selection
 * - Entries are removed when priority falls below 2%
 */

#include "play_scheduler_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "ps_nae";

#define NAE_INITIAL_PRIORITY 0.50f
#define NAE_MIN_PRIORITY 0.02f

// ============================================================================
// Pool Management
// ============================================================================

/**
 * @brief Find entry by artwork_id
 * @return Index of entry, or -1 if not found
 */
static int find_entry(ps_state_t *state, int32_t artwork_id)
{
    for (size_t i = 0; i < state->nae_count; i++) {
        if (state->nae_pool[i].artwork.artwork_id == artwork_id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Find entry with lowest priority (for eviction)
 */
static int find_lowest_priority(ps_state_t *state)
{
    if (state->nae_count == 0) return -1;

    int lowest = 0;
    float lowest_priority = state->nae_pool[0].priority;
    uint64_t oldest_time = state->nae_pool[0].insertion_time;

    for (size_t i = 1; i < state->nae_count; i++) {
        if (state->nae_pool[i].priority < lowest_priority ||
            (state->nae_pool[i].priority == lowest_priority &&
             state->nae_pool[i].insertion_time < oldest_time)) {
            lowest = (int)i;
            lowest_priority = state->nae_pool[i].priority;
            oldest_time = state->nae_pool[i].insertion_time;
        }
    }

    return lowest;
}

/**
 * @brief Find entry with highest priority (for selection)
 */
static int find_highest_priority(ps_state_t *state)
{
    if (state->nae_count == 0) return -1;

    int highest = 0;
    float highest_priority = state->nae_pool[0].priority;
    uint64_t oldest_time = state->nae_pool[0].insertion_time;

    for (size_t i = 1; i < state->nae_count; i++) {
        if (state->nae_pool[i].priority > highest_priority ||
            (state->nae_pool[i].priority == highest_priority &&
             state->nae_pool[i].insertion_time < oldest_time)) {
            highest = (int)i;
            highest_priority = state->nae_pool[i].priority;
            oldest_time = state->nae_pool[i].insertion_time;
        }
    }

    return highest;
}

/**
 * @brief Remove entry at index
 */
static void remove_entry(ps_state_t *state, size_t index)
{
    if (index >= state->nae_count) return;

    // Move last entry to this position (swap and pop)
    if (index < state->nae_count - 1) {
        state->nae_pool[index] = state->nae_pool[state->nae_count - 1];
    }
    state->nae_count--;
}

// ============================================================================
// Public API
// ============================================================================

void ps_nae_insert(ps_state_t *state, const ps_artwork_t *artwork)
{
    if (!state || !artwork || !state->nae_enabled) {
        return;
    }

    ESP_LOGD(TAG, "NAE insert: artwork_id=%d", (int)artwork->artwork_id);

    // Check if already exists
    int existing = find_entry(state, artwork->artwork_id);
    if (existing >= 0) {
        // Merge: reset priority to 50%
        state->nae_pool[existing].priority = NAE_INITIAL_PRIORITY;
        state->nae_pool[existing].insertion_time = (uint64_t)esp_timer_get_time();
        ESP_LOGD(TAG, "NAE merge: reset priority for artwork_id=%d", (int)artwork->artwork_id);
        return;
    }

    // Check if pool is full
    if (state->nae_count >= PS_NAE_POOL_SIZE) {
        // Evict entry with lowest priority
        int to_evict = find_lowest_priority(state);
        if (to_evict >= 0) {
            ESP_LOGD(TAG, "NAE evict: artwork_id=%d (priority=%.2f)",
                     (int)state->nae_pool[to_evict].artwork.artwork_id,
                     state->nae_pool[to_evict].priority);
            remove_entry(state, (size_t)to_evict);
        }
    }

    // Add new entry
    if (state->nae_count < PS_NAE_POOL_SIZE) {
        ps_nae_entry_t *entry = &state->nae_pool[state->nae_count];
        memcpy(&entry->artwork, artwork, sizeof(ps_artwork_t));
        entry->priority = NAE_INITIAL_PRIORITY;
        entry->insertion_time = (uint64_t)esp_timer_get_time();
        state->nae_count++;

        ESP_LOGI(TAG, "NAE added: artwork_id=%d (pool size=%zu)",
                 (int)artwork->artwork_id, state->nae_count);
    }
}

bool ps_nae_try_select(ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (!state || !out_artwork || !state->nae_enabled || state->nae_count == 0) {
        return false;
    }

    // Calculate total priority P = min(1, Σpi)
    float P = 0;
    for (size_t i = 0; i < state->nae_count; i++) {
        P += state->nae_pool[i].priority;
    }
    if (P > 1.0f) P = 1.0f;

    // Random check using PRNG
    float r = (float)ps_prng_next(&state->prng_nae_state) / (float)UINT32_MAX;
    if (r >= P) {
        // NAE not triggered
        return false;
    }

    // Select entry with highest priority
    int selected = find_highest_priority(state);
    if (selected < 0) {
        return false;
    }

    // Copy artwork
    memcpy(out_artwork, &state->nae_pool[selected].artwork, sizeof(ps_artwork_t));

    // Decay priority
    state->nae_pool[selected].priority /= 2.0f;

    ESP_LOGD(TAG, "NAE selected: artwork_id=%d, new priority=%.2f",
             (int)out_artwork->artwork_id, state->nae_pool[selected].priority);

    // Remove if priority too low
    if (state->nae_pool[selected].priority < NAE_MIN_PRIORITY) {
        ESP_LOGD(TAG, "NAE remove (priority too low): artwork_id=%d",
                 (int)out_artwork->artwork_id);
        remove_entry(state, (size_t)selected);
    }

    return true;
}

void ps_nae_clear(ps_state_t *state)
{
    if (!state) return;

    if (state->nae_count > 0) {
        ESP_LOGI(TAG, "NAE clear: removing %zu entries", state->nae_count);
    }
    state->nae_count = 0;
}
