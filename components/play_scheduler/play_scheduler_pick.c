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
#include "makapix_channel_impl.h"
#include "makapix_channel_internal.h"
#include "sd_path.h"
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

static asset_type_t get_asset_type_from_extension(uint8_t ext)
{
    switch (ext) {
        case 0: return ASSET_TYPE_WEBP;
        case 1: return ASSET_TYPE_GIF;
        case 2: return ASSET_TYPE_PNG;
        case 3: return ASSET_TYPE_JPEG;
        default: return ASSET_TYPE_WEBP;
    }
}

static const char *s_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

/**
 * @brief Build vault filepath for an entry
 *
 * Uses SHA256 sharding: {vault}/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
 */
static void ps_build_vault_filepath(const makapix_channel_entry_t *entry,
                                     char *out, size_t out_len)
{
    if (!entry || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    // Get vault base path
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        strlcpy(vault_base, "/sdcard/p3a/vault", sizeof(vault_base));
    }

    // Convert UUID bytes to string
    char storage_key[40];
    bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

    // Compute SHA256 for sharding
    uint8_t sha256[32];
    if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
        // Fallback without sharding
        int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
        snprintf(out, out_len, "%s/%s%s", vault_base, storage_key, s_ext_strings[ext_idx]);
        return;
    }

    // Build sharded path
    int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
    snprintf(out, out_len, "%s/%02x/%02x/%02x/%s%s",
             vault_base,
             (unsigned int)sha256[0],
             (unsigned int)sha256[1],
             (unsigned int)sha256[2],
             storage_key,
             s_ext_strings[ext_idx]);
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
    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;

    if (!entries || ch->entry_count == 0) {
        return false;
    }

    // Maximum attempts to find available artwork (skip unavailable ones)
    size_t max_attempts = ch->entry_count;
    if (max_attempts > 10) max_attempts = 10;  // Limit retries

    for (size_t attempt = 0; attempt < max_attempts; attempt++) {
        // Wrap cursor
        if (ch->cursor >= ch->entry_count) {
            ch->cursor = 0;
        }

        makapix_channel_entry_t *entry = &entries[ch->cursor];

        // Skip playlists for now (only handle artwork posts)
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            ch->cursor++;
            continue;
        }

        // Build filepath for this entry
        char filepath[256];
        ps_build_vault_filepath(entry, filepath, sizeof(filepath));

        // Check if file exists
        if (!file_exists(filepath)) {
            ch->cursor++;
            ESP_LOGD(TAG, "Skipping unavailable file: %s", filepath);
            continue;
        }

        // Check immediate repeat
        if (entry->post_id == state->last_played_id && attempt < 2) {
            ch->cursor++;
            continue;
        }

        // Found valid artwork - build storage_key string
        char storage_key[40];
        bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        strlcpy(out_artwork->storage_key, storage_key, sizeof(out_artwork->storage_key));
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = entry->dwell_time_ms;
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        // Advance cursor for next pick
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
    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;

    if (!entries || ch->entry_count == 0) {
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

        makapix_channel_entry_t *entry = &entries[index];

        // Only handle single artworks for now (not playlists)
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }

        // Build filepath for this entry
        char filepath[256];
        ps_build_vault_filepath(entry, filepath, sizeof(filepath));

        // Check if file exists
        if (!file_exists(filepath)) {
            continue;
        }

        // Check immediate repeat
        if (entry->post_id == state->last_played_id && attempt < 4) {
            continue;  // Resample
        }

        // Found valid artwork - build storage_key string
        char storage_key[40];
        bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        strlcpy(out_artwork->storage_key, storage_key, sizeof(out_artwork->storage_key));
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = entry->dwell_time_ms;
        out_artwork->type = get_asset_type_from_extension(entry->extension);
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
