// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_buffers.c
 * @brief History and Lookahead buffer management
 *
 * Implements ring buffer for history and FIFO queue for lookahead.
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

    // history_position: -1 means at head, 0..N means steps back from head
    // Can go back if we haven't reached the oldest entry
    int32_t steps_back = state->history_position + 1;
    return (size_t)(steps_back + 1) <= state->history_count;
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

    // Calculate index: head is at history_head-1, going back means lower indices
    size_t steps_from_head = (size_t)(state->history_position + 1);
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

    if (state->history_position < 0) {
        // At head - no artwork to return from history (need to use lookahead)
        return false;
    }

    // Calculate index
    size_t steps_from_head = (size_t)(state->history_position + 1);
    size_t idx = (state->history_head + PS_HISTORY_SIZE - steps_from_head) % PS_HISTORY_SIZE;

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
        // At head - get most recent entry
        size_t idx = (state->history_head + PS_HISTORY_SIZE - 1) % PS_HISTORY_SIZE;
        memcpy(out_artwork, &state->history[idx], sizeof(ps_artwork_t));
    } else {
        // In history - get entry at current position
        size_t steps_from_head = (size_t)(state->history_position + 1);
        size_t idx = (state->history_head + PS_HISTORY_SIZE - steps_from_head) % PS_HISTORY_SIZE;
        memcpy(out_artwork, &state->history[idx], sizeof(ps_artwork_t));
    }

    return true;
}

bool ps_history_is_at_head(const ps_state_t *state)
{
    return state->history_position < 0;
}

// ============================================================================
// Lookahead Buffer Operations
// ============================================================================

void ps_lookahead_init(ps_state_t *state)
{
    state->lookahead_head = 0;
    state->lookahead_tail = 0;
    state->lookahead_count = 0;
}

void ps_lookahead_clear(ps_state_t *state)
{
    state->lookahead_head = 0;
    state->lookahead_tail = 0;
    state->lookahead_count = 0;
}

bool ps_lookahead_is_empty(const ps_state_t *state)
{
    return state->lookahead_count == 0;
}

bool ps_lookahead_is_low(const ps_state_t *state)
{
    return state->lookahead_count < PS_LOOKAHEAD_SIZE;
}

size_t ps_lookahead_count(const ps_state_t *state)
{
    return state->lookahead_count;
}

bool ps_lookahead_push(ps_state_t *state, const ps_artwork_t *artwork)
{
    if (!state->lookahead || !artwork) return false;

    // Check if full
    if (state->lookahead_count >= PS_LOOKAHEAD_SIZE) {
        return false;
    }

    // Add at tail
    memcpy(&state->lookahead[state->lookahead_tail], artwork, sizeof(ps_artwork_t));
    state->lookahead_tail = (state->lookahead_tail + 1) % PS_LOOKAHEAD_SIZE;
    state->lookahead_count++;

    return true;
}

bool ps_lookahead_pop(ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (state->lookahead_count == 0) {
        return false;
    }

    // Get from head
    if (out_artwork) {
        memcpy(out_artwork, &state->lookahead[state->lookahead_head], sizeof(ps_artwork_t));
    }

    state->lookahead_head = (state->lookahead_head + 1) % PS_LOOKAHEAD_SIZE;
    state->lookahead_count--;

    return true;
}

bool ps_lookahead_peek(const ps_state_t *state, size_t index, ps_artwork_t *out_artwork)
{
    if (index >= state->lookahead_count || !out_artwork) {
        return false;
    }

    size_t idx = (state->lookahead_head + index) % PS_LOOKAHEAD_SIZE;
    memcpy(out_artwork, &state->lookahead[idx], sizeof(ps_artwork_t));

    return true;
}

size_t ps_lookahead_peek_many(const ps_state_t *state, size_t max_count,
                               ps_artwork_t *out_artworks)
{
    if (!out_artworks || state->lookahead_count == 0) {
        return 0;
    }

    size_t count = (max_count < state->lookahead_count) ? max_count : state->lookahead_count;

    for (size_t i = 0; i < count; i++) {
        size_t idx = (state->lookahead_head + i) % PS_LOOKAHEAD_SIZE;
        memcpy(&out_artworks[i], &state->lookahead[idx], sizeof(ps_artwork_t));
    }

    return count;
}

bool ps_lookahead_rotate(ps_state_t *state)
{
    if (state->lookahead_count <= 1) {
        return false;
    }

    // Move head item to tail (skip without removing permanently)
    // Items: [head, head+1, ..., tail-1] -> [head+1, ..., tail-1, head]

    // Save head item
    ps_artwork_t temp;
    memcpy(&temp, &state->lookahead[state->lookahead_head], sizeof(ps_artwork_t));

    // Put at current tail position
    memcpy(&state->lookahead[state->lookahead_tail], &temp, sizeof(ps_artwork_t));

    // Advance both head and tail
    state->lookahead_head = (state->lookahead_head + 1) % PS_LOOKAHEAD_SIZE;
    state->lookahead_tail = (state->lookahead_tail + 1) % PS_LOOKAHEAD_SIZE;

    // Count stays the same
    return true;
}
