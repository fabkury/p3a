// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_pick.c
 * @brief Per-channel artwork picking logic
 *
 * Implements RecencyPick and RandomPick modes.
 */

#include "play_scheduler_internal.h"
#include "channel_interface.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "ps_pick";

// ============================================================================
// Utilities
// ============================================================================

static bool file_exists(const char *path)
{
    if (!path || path[0] == '\0') return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static asset_type_t get_asset_type_from_filepath(const char *filepath)
{
    if (!filepath) return ASSET_TYPE_WEBP;

    const char *ext = strrchr(filepath, '.');
    if (!ext) return ASSET_TYPE_WEBP;

    if (strcasecmp(ext, ".webp") == 0) return ASSET_TYPE_WEBP;
    if (strcasecmp(ext, ".gif") == 0) return ASSET_TYPE_GIF;
    if (strcasecmp(ext, ".png") == 0) return ASSET_TYPE_PNG;
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return ASSET_TYPE_JPEG;

    return ASSET_TYPE_WEBP;
}

// ============================================================================
// PRNG (Simple PCG32)
// ============================================================================

uint32_t ps_prng_next(uint32_t *state)
{
    uint64_t oldstate = *state;
    *state = (uint32_t)((oldstate * 6364136223846793005ULL + 1) & 0xFFFFFFFF);
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

void ps_prng_seed(uint32_t *state, uint32_t seed)
{
    *state = seed;
    ps_prng_next(state);  // Advance once
}

// ============================================================================
// RecencyPick Mode
// ============================================================================

static bool pick_recency(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];
    channel_handle_t handle = (channel_handle_t)ch->handle;

    if (!handle || ch->entry_count == 0) {
        return false;
    }

    // Get item at current cursor position
    channel_item_ref_t item;
    esp_err_t err;

    // Maximum attempts to find available artwork (skip unavailable ones)
    size_t max_attempts = ch->entry_count;
    if (max_attempts > 10) max_attempts = 10;  // Limit retries

    for (size_t attempt = 0; attempt < max_attempts; attempt++) {
        // Wrap cursor
        if (ch->cursor >= ch->entry_count) {
            ch->cursor = 0;
        }

        err = channel_current_item(handle, &item);
        if (err != ESP_OK) {
            // Try advancing
            channel_next_item(handle, &item);
            ch->cursor++;
            continue;
        }

        // Check if file exists
        if (!file_exists(item.filepath)) {
            // Skip to next
            channel_next_item(handle, &item);
            ch->cursor++;

            // Count skips for repeat avoidance
            if (attempt < 2) continue;  // Skip up to 2 records before accepting
            ESP_LOGD(TAG, "Skipped %zu unavailable files", attempt);
            continue;
        }

        // Check immediate repeat
        if (item.post_id == state->last_played_id && attempt < 2) {
            // Skip on immediate repeat (up to 2 skips)
            channel_next_item(handle, &item);
            ch->cursor++;
            continue;
        }

        // Found valid artwork
        out_artwork->artwork_id = item.post_id;
        out_artwork->post_id = item.post_id;
        strlcpy(out_artwork->filepath, item.filepath, sizeof(out_artwork->filepath));
        strlcpy(out_artwork->storage_key, item.storage_key, sizeof(out_artwork->storage_key));
        out_artwork->created_at = 0;  // Not available in channel_item_ref_t
        out_artwork->dwell_time_ms = item.dwell_time_ms;
        out_artwork->type = get_asset_type_from_filepath(item.filepath);
        out_artwork->channel_index = (uint8_t)channel_index;

        // Advance cursor for next pick
        channel_next_item(handle, &item);
        ch->cursor++;

        return true;
    }

    ESP_LOGW(TAG, "RecencyPick: No available artwork found after %zu attempts", max_attempts);
    return false;
}

// ============================================================================
// RandomPick Mode
// ============================================================================

static bool pick_random(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];
    channel_handle_t handle = (channel_handle_t)ch->handle;

    if (!handle || ch->entry_count == 0) {
        return false;
    }

    // R_eff = min(R, Mi)
    size_t r_eff = PS_RANDOM_WINDOW;
    if (r_eff > ch->entry_count) {
        r_eff = ch->entry_count;
    }

    // Try up to 5 resample attempts for repeat avoidance
    for (int attempt = 0; attempt < 5; attempt++) {
        // Sample uniformly from newest r_eff records
        uint32_t r = ps_prng_next(&ch->pick_rng_state);
        size_t index = (size_t)(r % r_eff);

        // Get post at this index
        channel_post_t post;
        esp_err_t err = channel_get_post(handle, index, &post);
        if (err != ESP_OK) {
            continue;
        }

        // Only handle single artworks for now (not playlists)
        if (post.kind != CHANNEL_POST_KIND_ARTWORK) {
            continue;
        }

        // Check if file exists
        if (!file_exists(post.u.artwork.filepath)) {
            continue;
        }

        // Check immediate repeat
        if (post.post_id == state->last_played_id && attempt < 4) {
            continue;  // Resample
        }

        // Found valid artwork
        out_artwork->artwork_id = post.post_id;
        out_artwork->post_id = post.post_id;
        strlcpy(out_artwork->filepath, post.u.artwork.filepath, sizeof(out_artwork->filepath));
        strlcpy(out_artwork->storage_key, post.u.artwork.storage_key, sizeof(out_artwork->storage_key));
        out_artwork->created_at = post.created_at;
        out_artwork->dwell_time_ms = post.dwell_time_ms;
        out_artwork->type = post.u.artwork.type;
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGW(TAG, "RandomPick: No available artwork found after 5 attempts");

    // Fall back to recency pick
    return pick_recency(state, channel_index, out_artwork);
}

// ============================================================================
// Public API
// ============================================================================

bool ps_pick_artwork(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    if (!state || !out_artwork || channel_index >= state->channel_count) {
        return false;
    }

    if (state->pick_mode == PS_PICK_RANDOM) {
        return pick_random(state, channel_index, out_artwork);
    } else {
        return pick_recency(state, channel_index, out_artwork);
    }
}

void ps_pick_reset_channel(ps_state_t *state, size_t channel_index)
{
    if (!state || channel_index >= state->channel_count) {
        return;
    }

    ps_channel_state_t *ch = &state->channels[channel_index];
    ch->cursor = 0;

    // Re-seed pick PRNG
    ps_prng_seed(&ch->pick_rng_state, state->global_seed ^ (uint32_t)channel_index ^ state->epoch_id);
}
