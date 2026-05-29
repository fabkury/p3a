// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file play_scheduler_swrr.c
 * @brief Smooth Weighted Round Robin (SWRR) channel scheduler
 *
 * Implements fair channel scheduling with configurable weights.
 * Uses Wsum = 65536 for integer arithmetic precision.
 */

#include "play_scheduler_internal.h"
#include "channel_cache.h"
#include "esp_log.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static const char *TAG = "ps_chsel";

#define WSUM 65536

// ============================================================================
// Helpers
// ============================================================================

/**
 * @brief Get effective count for a channel (available for Makapix, entry for SD)
 *
 * For Makapix channels, uses available_count (LAi) since those are the only
 * artworks that can actually be picked. For SD card channels, uses entry_count.
 */
static inline size_t get_effective_count(const ps_channel_state_t *ch)
{
    if ((ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX || ch->entry_format == PS_ENTRY_FORMAT_GIPHY || ch->entry_format == PS_ENTRY_FORMAT_INSTITUTION) && ch->cache) {
        return ch->cache->available_count;
    }
    return ch->entry_count;
}

/**
 * @brief Check if channel has available artwork
 */
static inline bool has_available_artwork(const ps_channel_state_t *ch)
{
    if (!ch->active) {
        return false;
    }
    // Makapix/Giphy: check cache directly; SD card: check entry_count
    size_t entry_count = ((ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX || ch->entry_format == PS_ENTRY_FORMAT_GIPHY || ch->entry_format == PS_ENTRY_FORMAT_INSTITUTION) && ch->cache)
                         ? ch->cache->entry_count
                         : ch->entry_count;
    if (entry_count == 0) {
        return false;
    }
    return get_effective_count(ch) > 0;
}

// ============================================================================
// Weight Calculation
// ============================================================================

/**
 * @brief Compute per-channel weights from spec_weights
 *
 * Sums spec_weights of active channels with available artwork. If the sum is
 * zero (e.g. user never set any weights), distributes WSUM equally among
 * active channels — the universal default. Otherwise normalizes spec_weights
 * to WSUM proportionally. Inactive or unavailable channels get weight 0.
 */
void ps_swrr_calculate_weights(ps_state_t *state)
{
    if (!state) return;

    uint32_t total_weight = 0;
    size_t active_count = 0;
    for (size_t i = 0; i < state->channel_count; i++) {
        if (has_available_artwork(&state->channels[i])) {
            total_weight += state->channels[i].spec_weight;
            active_count++;
        }
    }

    if (active_count == 0) {
        // Nothing to do; leave weights as-is.
    } else if (total_weight == 0) {
        // Fall back to equal distribution
        uint32_t weight_per_channel = WSUM / active_count;
        for (size_t i = 0; i < state->channel_count; i++) {
            state->channels[i].weight = has_available_artwork(&state->channels[i])
                ? weight_per_channel : 0;
        }
    } else {
        // Normalize spec_weight to WSUM
        for (size_t i = 0; i < state->channel_count; i++) {
            if (has_available_artwork(&state->channels[i])) {
                state->channels[i].weight =
                    (uint32_t)((uint64_t)state->channels[i].spec_weight * WSUM / total_weight);
            } else {
                state->channels[i].weight = 0;
            }
        }
    }

    // Log weights
    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        if ((ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX || ch->entry_format == PS_ENTRY_FORMAT_GIPHY || ch->entry_format == PS_ENTRY_FORMAT_INSTITUTION) && ch->cache) {
            ESP_LOGD(TAG, "Channel '%s': weight=%lu, active=%d, entries=%zu, available=%zu",
                     ch->display_name,
                     (unsigned long)ch->weight,
                     ch->active,
                     ch->cache->entry_count,
                     ch->cache->available_count);
        } else {
            ESP_LOGD(TAG, "Channel '%s': weight=%lu, active=%d, entries=%zu (SD card)",
                     ch->display_name,
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

    // Find channel(s) with maximum credit, break ties randomly
    int candidates[PS_MAX_CHANNELS];
    int candidate_count = 0;
    int32_t best_credit = INT32_MIN;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (!state->channels[i].active || state->channels[i].weight == 0) {
            continue;
        }

        if (state->channels[i].credit > best_credit) {
            best_credit = state->channels[i].credit;
            candidates[0] = (int)i;
            candidate_count = 1;
        } else if (state->channels[i].credit == best_credit) {
            candidates[candidate_count++] = (int)i;
        }
    }

    int best = -1;
    if (candidate_count == 1) {
        best = candidates[0];
    } else if (candidate_count > 1) {
        uint32_t r = ps_prng_next(&state->prng_pick_state) % (uint32_t)candidate_count;
        best = candidates[r];
    }

    // Deduct WSUM from selected channel
    if (best >= 0) {
        state->channels[best].credit -= WSUM;

        // Log SWRR selection with all channel credits
        ESP_LOGI(TAG, "SWRR selected channel[%d] '%s' (credit was %ld, now %ld)",
                 best, state->channels[best].display_name,
                 (long)(best_credit), (long)state->channels[best].credit);

        // Log all channel credits for debugging
        for (size_t i = 0; i < state->channel_count; i++) {
            ps_channel_state_t *ch = &state->channels[i];
            if (ch->active && ch->weight > 0) {
                size_t eff_count = get_effective_count(ch);
                ESP_LOGD(TAG, "  SWRR ch[%zu] '%s': credit=%ld, weight=%lu, eff_count=%zu",
                         i, ch->display_name, (long)ch->credit,
                         (unsigned long)ch->weight, eff_count);
            }
        }
    }

    return best;
}

// ============================================================================
// Stochastic Channel Selection
// ============================================================================

int ps_stochastic_select_channel(ps_state_t *state)
{
    if (!state || state->channel_count == 0) {
        return -1;
    }

    // Credit accumulation identical to SWRR
    for (size_t i = 0; i < state->channel_count; i++) {
        if (state->channels[i].active && state->channels[i].weight > 0) {
            state->channels[i].credit += (int32_t)state->channels[i].weight;
        }
    }

    // Compute selection probability: P(i) = w_i * clamp(1 + alpha * credit_i / WSUM, 0.1, 3.0)
    const float alpha = 0.8f;
    const float floor_val = 0.1f;
    const float ceil_val = 3.0f;

    float probs[PS_MAX_CHANNELS];
    float sum = 0;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (!state->channels[i].active || state->channels[i].weight == 0) {
            probs[i] = 0;
            continue;
        }

        float credit_factor = 1.0f + alpha * (float)state->channels[i].credit / (float)WSUM;
        if (credit_factor < floor_val) credit_factor = floor_val;
        if (credit_factor > ceil_val) credit_factor = ceil_val;

        probs[i] = (float)state->channels[i].weight * credit_factor;
        sum += probs[i];
    }

    if (sum <= 0) {
        return -1;
    }

    // Sample using PRNG
    float r = (float)ps_prng_next(&state->prng_pick_state) / (float)UINT32_MAX * sum;
    float cumulative = 0;
    int best = -1;

    for (size_t i = 0; i < state->channel_count; i++) {
        if (probs[i] <= 0) continue;
        cumulative += probs[i];
        if (r <= cumulative) {
            best = (int)i;
            break;
        }
    }

    // Fallback: last active channel (handles float rounding edge case)
    if (best < 0) {
        for (int i = (int)state->channel_count - 1; i >= 0; i--) {
            if (state->channels[i].active && state->channels[i].weight > 0) {
                best = i;
                break;
            }
        }
    }

    // Credit deduction identical to SWRR
    if (best >= 0) {
        int32_t prev_credit = state->channels[best].credit;
        state->channels[best].credit -= WSUM;

        ESP_LOGI(TAG, "Stochastic selected channel[%d] '%s' (credit was %ld, now %ld)",
                 best, state->channels[best].display_name,
                 (long)prev_credit, (long)state->channels[best].credit);

        for (size_t i = 0; i < state->channel_count; i++) {
            ps_channel_state_t *ch = &state->channels[i];
            if (ch->active && ch->weight > 0) {
                size_t eff_count = get_effective_count(ch);
                ESP_LOGD(TAG, "  Stoch ch[%zu] '%s': credit=%ld, weight=%lu, prob=%.1f%%, eff_count=%zu",
                         i, ch->display_name, (long)ch->credit,
                         (unsigned long)ch->weight,
                         (double)(probs[i] / sum * 100.0f),
                         eff_count);
            }
        }
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

// ============================================================================
// Credit refund (cross-task)
// ============================================================================

void play_scheduler_refund_swrr_credit(ps_channel_type_t type,
                                       const char *spec_name,
                                       const char *identifier)
{
    ps_state_t *state = ps_get_state();
    if (!state || !state->mutex || !state->initialized) {
        return;
    }
    if (!spec_name)  spec_name = "";
    if (!identifier) identifier = "";

    xSemaphoreTake(state->mutex, portMAX_DELAY);
    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        if (ch->type == type &&
            strcmp(ch->spec_name, spec_name) == 0 &&
            strcmp(ch->identifier, identifier) == 0) {
            int32_t before = ch->credit;
            ch->credit += WSUM;
            ESP_LOGI(TAG,
                "Refunded SWRR credit to channel[%zu] '%s' after corrupt file (credit %ld -> %ld)",
                i, ch->display_name, (long)before, (long)ch->credit);
            break;
        }
    }
    xSemaphoreGive(state->mutex);
}
