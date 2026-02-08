// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_buffers.c
 * @brief History buffer management
 *
 * Implements ring buffer for history (back-navigation).
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include <string.h>

// ============================================================================
// History Buffer Operations
// ============================================================================

void ps_history_init(ps_state_t *state)
{
    state->history_head = 0;
    state->history_count = 0;
    state->history_position = -1;  // At head (most recent)
}

void ps_history_clear(ps_state_t *state)
{
    state->history_head = 0;
    state->history_count = 0;
    state->history_position = -1;
}

void ps_history_push(ps_state_t *state, const ps_artwork_t *artwork)
{
    if (!state->history || !artwork) return;

    // Deduplicate: skip if identical to the most recent entry
    if (state->history_count > 0) {
        size_t last_idx = (state->history_head + PS_HISTORY_SIZE - 1) % PS_HISTORY_SIZE;
        if (strcmp(state->history[last_idx].filepath, artwork->filepath) == 0) {
            return;
        }
    }

    // Add to ring buffer at head position
    size_t idx = state->history_head;
    memcpy(&state->history[idx], artwork, sizeof(ps_artwork_t));

    // Advance head
    state->history_head = (state->history_head + 1) % PS_HISTORY_SIZE;

    // Track count (max out at buffer size)
    if (state->history_count < PS_HISTORY_SIZE) {
        state->history_count++;
    }

    // Reset position to head (most recent)
    state->history_position = -1;
}

bool ps_history_can_go_back(const ps_state_t *state)
{
    if (state->history_count == 0) return false;

    // history_position: -1 means at head (viewing current), 0+ means steps back
    // After going back, we'll be at position+1 viewing entry at steps_from_head = position+2
    // Need that entry to exist: (position + 2) < history_count, i.e., position + 3 <= count
    // Equivalently: need at least 2 entries when at head, 3 when at pos 0, etc.
    int32_t new_steps_from_head = state->history_position + 2;
    return (size_t)(new_steps_from_head + 1) <= state->history_count;
}

bool ps_history_can_go_forward(const ps_state_t *state)
{
    // Can go forward if we're not at head (-1)
    return state->history_position >= 0;
}

bool ps_history_go_back(ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (!ps_history_can_go_back(state)) {
        return false;
    }

    // Move position back
    state->history_position++;

    // Calculate index: history_head-1 is current, history_head-2 is previous, etc.
    // At position 0, we want the previous artwork (steps_from_head = 2)
    // At position 1, we want one before that (steps_from_head = 3)
    size_t steps_from_head = (size_t)(state->history_position + 2);
    size_t idx = (state->history_head + PS_HISTORY_SIZE - steps_from_head) % PS_HISTORY_SIZE;

    if (out_artwork) {
        memcpy(out_artwork, &state->history[idx], sizeof(ps_artwork_t));
    }

    return true;
}

bool ps_history_go_forward(ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (!ps_history_can_go_forward(state)) {
        return false;
    }

    // Move position forward (toward head)
    state->history_position--;

    size_t idx;
    if (state->history_position < 0) {
        // Back at head - return the most recent entry (current artwork)
        idx = (state->history_head + PS_HISTORY_SIZE - 1) % PS_HISTORY_SIZE;
    } else {
        // Still in history - calculate index (same scheme as go_back: position + 2)
        size_t steps_from_head = (size_t)(state->history_position + 2);
        idx = (state->history_head + PS_HISTORY_SIZE - steps_from_head) % PS_HISTORY_SIZE;
    }

    if (out_artwork) {
        memcpy(out_artwork, &state->history[idx], sizeof(ps_artwork_t));
    }

    return true;
}

bool ps_history_get_current(const ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (state->history_count == 0 || !out_artwork) {
        return false;
    }

    if (state->history_position < 0) {
        // At head - get most recent entry (current artwork)
        size_t idx = (state->history_head + PS_HISTORY_SIZE - 1) % PS_HISTORY_SIZE;
        memcpy(out_artwork, &state->history[idx], sizeof(ps_artwork_t));
    } else {
        // In history - get entry at current position (same scheme: position + 2)
        size_t steps_from_head = (size_t)(state->history_position + 2);
        size_t idx = (state->history_head + PS_HISTORY_SIZE - steps_from_head) % PS_HISTORY_SIZE;
        memcpy(out_artwork, &state->history[idx], sizeof(ps_artwork_t));
    }

    return true;
}

bool ps_history_is_at_head(const ps_state_t *state)
{
    return state->history_position < 0;
}
