// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_swrr.c
 * @brief Smooth Weighted Round Robin (SWRR) channel scheduler
 *
 * Implements fair channel scheduling with configurable weights.
 * Uses Wsum = 65536 for integer arithmetic precision.
 */

#include "play_scheduler_internal.h"
#include "esp_log.h"
#include <limits.h>
#include <math.h>

static const char *TAG = "ps_swrr";

#define WSUM 65536

// ============================================================================
// Helpers
// ============================================================================

/**
 * @brief Get effective count for a channel (available for Makapix, entry for SD)
 *
 * For Makapix channels, uses available_count (LAi) since those are the only
 * artworks that can actually be picked. For SD card channels (no LAi yet),
 * uses entry_count.
 */
static inline size_t get_effective_count(const ps_channel_state_t *ch)
{
    if (ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX) {
        return ch->available_count;
    }
    return ch->entry_count;
}

/**
 * @brief Check if channel has available artwork
 */
static inline bool has_available_artwork(const ps_channel_state_t *ch)
{
    if (!ch->active || ch->entry_count == 0) {
        return false;
    }
    return get_effective_count(ch) > 0;
}

// ============================================================================
// Weight Calculation
// ============================================================================

/**
 * @brief Calculate EqE (Equal Exposure) weights
 */
static void calculate_weights_equal(ps_state_t *state)
{
    size_t active_count = 0;

    // Count active channels with available artwork
    for (size_t i = 0; i < state->channel_count; i++) {
        if (has_available_artwork(&state->channels[i])) {
            active_count++;
        }
    }

    if (active_count == 0) {
        return;
    }

    // Equal weight for each active channel with available artwork
    uint32_t weight_per_channel = WSUM / active_count;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (has_available_artwork(&state->channels[i])) {
            state->channels[i].weight = weight_per_channel;
        } else {
            state->channels[i].weight = 0;
        }
    }
}

/**
 * @brief Calculate MaE (Manual Exposure) weights
 */
static void calculate_weights_manual(ps_state_t *state)
{
    uint32_t total_weight = 0;

    // Sum manual weights for active channels with available artwork
    for (size_t i = 0; i < state->channel_count; i++) {
        if (has_available_artwork(&state->channels[i])) {
            total_weight += state->channels[i].weight;
        }
    }

    if (total_weight == 0) {
        // Fall back to equal weights
        calculate_weights_equal(state);
        return;
    }

    // Normalize to WSUM
    for (size_t i = 0; i < state->channel_count; i++) {
        if (has_available_artwork(&state->channels[i])) {
            state->channels[i].weight =
                (uint32_t)((uint64_t)state->channels[i].weight * WSUM / total_weight);
        } else {
            state->channels[i].weight = 0;
        }
    }
}

/**
 * @brief Calculate PrE (Proportional Exposure) weights with recency bias
 *
 * Parameters from specification:
 * - α = 0.35 (recency blend factor)
 * - p_min = 0.02
 * - p_max = 0.40
 *
 * Uses available_count (LAi) for Makapix channels to ensure weights
 * reflect what can actually be played.
 */
static void calculate_weights_proportional(ps_state_t *state)
{
    const float alpha = 0.35f;
    const float p_min = 0.02f;
    const float p_max = 0.40f;

    // Calculate totals using effective counts (available for Makapix)
    uint64_t sum_total = 0;
    uint64_t sum_recent = 0;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (has_available_artwork(&state->channels[i])) {
            size_t eff_count = get_effective_count(&state->channels[i]);
            sum_total += eff_count;
            // For now, use eff_count as recent_count approximation
            // In future, this should come from server data
            sum_recent += eff_count / 4;  // Assume 25% is recent
        }
    }

    if (sum_total == 0) {
        return;
    }

    float weights[PS_MAX_CHANNELS] = {0};
    float sum_clamped = 0;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (!has_available_artwork(&state->channels[i])) {
            weights[i] = 0;
            continue;
        }

        size_t eff_count = get_effective_count(&state->channels[i]);

        // Normalize using effective counts
        float p_total = (float)eff_count / (float)sum_total;
        float p_recent = (sum_recent > 0)
            ? (float)(eff_count / 4) / (float)sum_recent
            : 0;

        // Blend
        float p_raw = (1.0f - alpha) * p_total + alpha * p_recent;

        // Clamp
        if (p_raw < p_min) p_raw = p_min;
        if (p_raw > p_max) p_raw = p_max;

        weights[i] = p_raw;
        sum_clamped += p_raw;
    }

    // Renormalize and convert to integer weights
    if (sum_clamped > 0) {
        for (size_t i = 0; i < state->channel_count; i++) {
            if (has_available_artwork(&state->channels[i])) {
                state->channels[i].weight = (uint32_t)(weights[i] / sum_clamped * WSUM);
            } else {
                state->channels[i].weight = 0;
            }
        }
    }
}

void ps_swrr_calculate_weights(ps_state_t *state)
{
    if (!state) return;

    ESP_LOGD(TAG, "Calculating weights for mode %d", state->exposure_mode);

    switch (state->exposure_mode) {
        case PS_EXPOSURE_EQUAL:
            calculate_weights_equal(state);
            break;
        case PS_EXPOSURE_MANUAL:
            calculate_weights_manual(state);
            break;
        case PS_EXPOSURE_PROPORTIONAL:
            calculate_weights_proportional(state);
            break;
        default:
            calculate_weights_equal(state);
            break;
    }

    // Log weights
    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        if (ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX) {
            ESP_LOGD(TAG, "Channel '%s': weight=%lu, active=%d, entries=%zu, available=%zu",
                     ch->channel_id,
                     (unsigned long)ch->weight,
                     ch->active,
                     ch->entry_count,
                     ch->available_count);
        } else {
            ESP_LOGD(TAG, "Channel '%s': weight=%lu, active=%d, entries=%zu (SD card)",
                     ch->channel_id,
                     (unsigned long)ch->weight,
                     ch->active,
                     ch->entry_count);
        }
    }
}

// ============================================================================
// Channel Selection
// ============================================================================

int ps_swrr_select_channel(ps_state_t *state)
{
    if (!state || state->channel_count == 0) {
        return -1;
    }

    // Add credits to all channels
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].active && state->channels[i].weight > 0) {
            state->channels[i].credit += (int32_t)state->channels[i].weight;
        }
    }

    // Find channel with maximum credit
    int best = -1;
    int32_t best_credit = INT32_MIN;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (!state->channels[i].active || state->channels[i].weight == 0) {
            continue;
        }

        if (state->channels[i].credit > best_credit) {
            best_credit = state->channels[i].credit;
            best = (int)i;
        }
        // Tie-break: lowest channel ID (already in order, so first wins)
    }

    // Deduct WSUM from selected channel
    if (best >= 0) {
        state->channels[best].credit -= WSUM;
    }

    return best;
}

void ps_swrr_reset_credits(ps_state_t *state)
{
    if (!state) return;

    for (size_t i = 0; i < state->channel_count; i++) {
        state->channels[i].credit = 0;
    }
}
