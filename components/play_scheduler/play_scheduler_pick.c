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
#include "makapix_channel_utils.h"
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

static bool has_404_marker(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') return false;
    char marker[264];
    snprintf(marker, sizeof(marker), "%s.404", filepath);
    struct stat st;
    return (stat(marker, &st) == 0);
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

/**
 * @brief Build filepath for an SD card entry
 *
 * Uses the filename field from sdcard_index_entry_t (144 bytes) to build
 * the full path: {animations_dir}/{filename}
 */
static void ps_build_sdcard_filepath(const sdcard_index_entry_t *entry,
                                      char *out, size_t out_len)
{
    if (!entry || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    // Get animations directory path
    char animations_path[128];
    if (sd_path_get_animations(animations_path, sizeof(animations_path)) != ESP_OK) {
        strlcpy(animations_path, "/sdcard/p3a/animations", sizeof(animations_path));
    }

    if (entry->filename[0] == '\0') {
        ESP_LOGD(TAG, "SD card entry has empty filename");
        out[0] = '\0';
        return;
    }

    // Build full path: animations_path + "/" + filename
    // Note: filename is max 143 chars, animations_path is max 127 chars
    // Total max: 127 + 1 + 143 + 1 = 272 bytes, but out_len is typically 256
    int written = snprintf(out, out_len, "%s/%s", animations_path, entry->filename);
    if (written < 0 || (size_t)written >= out_len) {
        ESP_LOGW(TAG, "SD card filepath truncated: %s", entry->filename);
    }
}

/**
 * @brief Build filepath for a Makapix entry
 *
 * Uses SHA256 sharding: {vault}/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
 */
void ps_build_vault_filepath(const makapix_channel_entry_t *entry,
                              char *out, size_t out_len)
{
    if (!entry || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    // Makapix entry - build vault path
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

/**
 * @brief Pick artwork from SD card channel using recency mode
 */
static bool pick_recency_sdcard(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];
    sdcard_index_entry_t *entries = (sdcard_index_entry_t *)ch->entries;

    if (!entries || ch->entry_count == 0) {
        ESP_LOGD(TAG, "pick_recency_sdcard: no entries (entries=%p, count=%zu)",
                 (void*)entries, ch->entry_count);
        return false;
    }

    ESP_LOGD(TAG, "pick_recency_sdcard: checking %zu entries, cursor=%lu",
             ch->entry_count, (unsigned long)ch->cursor);

    uint32_t start_cursor = ch->cursor;
    bool wrapped = false;

    while (true) {
        if (ch->cursor >= ch->entry_count) {
            if (wrapped) break;
            ch->cursor = 0;
            wrapped = true;
        }

        if (wrapped && ch->cursor >= start_cursor) break;

        sdcard_index_entry_t *entry = &entries[ch->cursor];
        ch->cursor++;

        // Build filepath for this entry
        char filepath[256];
        ps_build_sdcard_filepath(entry, filepath, sizeof(filepath));

        ESP_LOGI(TAG, "SD card entry[%lu]: filename='%s', path='%s'",
                 (unsigned long)(ch->cursor - 1), entry->filename, filepath);

        // AVAILABILITY MASKING: skip if file doesn't exist
        if (!file_exists(filepath)) {
            ESP_LOGW(TAG, "SD card file not found: %s", filepath);
            continue;
        }

        // Skip immediate repeat (but allow if only 1 entry in channel)
        if (entry->post_id == state->last_played_id && ch->entry_count > 1) {
            ESP_LOGD(TAG, "Skipping repeat: post_id=%ld == last_played_id=%ld",
                     (long)entry->post_id, (long)state->last_played_id);
            continue;
        }

        ESP_LOGI(TAG, "Found SD card artwork: post_id=%ld, path=%s",
                 (long)entry->post_id, filepath);

        // Found valid artwork
        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        out_artwork->storage_key[0] = '\0';  // No storage key for local files
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = entry->dwell_time_ms;
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGD(TAG, "RecencyPick: SD card channel %zu exhausted", channel_index);
    return false;
}

/**
 * @brief Pick artwork from Makapix channel using recency mode (LAi-based)
 *
 * Uses LAi (available_indices) for O(1) availability checking.
 * Iterates through locally available artworks only.
 */
static bool pick_recency_makapix(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];
    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;

    if (!entries || ch->entry_count == 0) {
        return false;
    }

    // Use LAi for availability masking - no filesystem I/O
    if (!ch->available_indices || ch->available_count == 0) {
        ESP_LOGD(TAG, "RecencyPick: Makapix channel %zu has no available artworks", channel_index);
        return false;
    }

    // Cursor operates over available_indices (LAi), not full Ci
    uint32_t start_cursor = ch->cursor;
    bool wrapped = false;

    while (true) {
        if (ch->cursor >= ch->available_count) {
            if (wrapped) break;
            ch->cursor = 0;
            wrapped = true;
        }

        if (wrapped && ch->cursor >= start_cursor) break;

        // Get Ci index from LAi
        uint32_t ci_index = ch->available_indices[ch->cursor];
        ch->cursor++;

        // Validate Ci index
        if (ci_index >= ch->entry_count) {
            ESP_LOGW(TAG, "Invalid LAi entry: ci_index=%lu >= entry_count=%zu",
                     (unsigned long)ci_index, ch->entry_count);
            continue;
        }

        makapix_channel_entry_t *entry = &entries[ci_index];

        // Skip non-artwork entries (playlists, etc.)
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }

        // Skip immediate repeat (but allow if only 1 available entry)
        if (entry->post_id == state->last_played_id && ch->available_count > 1) {
            continue;
        }

        // Build filepath for this entry
        char filepath[256];
        ps_build_vault_filepath(entry, filepath, sizeof(filepath));

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

    ESP_LOGD(TAG, "RecencyPick: Makapix channel %zu exhausted (available=%zu)",
             channel_index, ch->available_count);
    return false;
}

/**
 * @brief Pick artwork using recency mode with availability masking
 *
 * Dispatches to format-specific implementation based on entry_format.
 */
static bool pick_recency(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];

    if (ch->entry_format == PS_ENTRY_FORMAT_SDCARD) {
        return pick_recency_sdcard(state, channel_index, out_artwork);
    } else {
        return pick_recency_makapix(state, channel_index, out_artwork);
    }
}

// ============================================================================
// RandomPick Mode
// ============================================================================

/**
 * @brief Pick random artwork from SD card channel
 */
static bool pick_random_sdcard(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];
    sdcard_index_entry_t *entries = (sdcard_index_entry_t *)ch->entries;

    if (!entries || ch->entry_count == 0) {
        return false;
    }

    // Sample from all entries for true shuffle
    size_t r_eff = ch->entry_count;

    for (int attempt = 0; attempt < 5; attempt++) {
        uint32_t r = ps_prng_next(&ch->pick_rng_state);
        size_t index = (size_t)(r % r_eff);

        sdcard_index_entry_t *entry = &entries[index];

        char filepath[256];
        ps_build_sdcard_filepath(entry, filepath, sizeof(filepath));

        if (!file_exists(filepath)) {
            continue;
        }

        // Skip immediate repeat (but allow if only 1 entry or after several attempts)
        if (entry->post_id == state->last_played_id && ch->entry_count > 1 && attempt < 4) {
            continue;
        }

        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        out_artwork->storage_key[0] = '\0';
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = entry->dwell_time_ms;
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGW(TAG, "RandomPick: SD card - no artwork after 5 attempts");
    return pick_recency_sdcard(state, channel_index, out_artwork);
}

/**
 * @brief Pick random artwork from Makapix channel (LAi-based)
 *
 * Uses LAi (available_indices) for O(1) random picks.
 * Samples directly from locally available artworks.
 */
static bool pick_random_makapix(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];
    makapix_channel_entry_t *entries = (makapix_channel_entry_t *)ch->entries;

    if (!entries || ch->entry_count == 0) {
        return false;
    }

    // Use LAi for O(1) random picks - no filesystem I/O
    if (!ch->available_indices || ch->available_count == 0) {
        ESP_LOGD(TAG, "RandomPick: Makapix channel %zu has no available artworks", channel_index);
        return false;
    }

    // Sample from available_indices (LAi) directly
    for (int attempt = 0; attempt < 5; attempt++) {
        uint32_t r = ps_prng_next(&ch->pick_rng_state);
        size_t lai_index = (size_t)(r % ch->available_count);
        uint32_t ci_index = ch->available_indices[lai_index];

        // Validate Ci index
        if (ci_index >= ch->entry_count) {
            ESP_LOGW(TAG, "Invalid LAi entry: ci_index=%lu >= entry_count=%zu",
                     (unsigned long)ci_index, ch->entry_count);
            continue;
        }

        makapix_channel_entry_t *entry = &entries[ci_index];

        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }

        // Skip immediate repeat (but allow if only 1 available or after several attempts)
        if (entry->post_id == state->last_played_id && ch->available_count > 1 && attempt < 4) {
            continue;
        }

        char filepath[256];
        ps_build_vault_filepath(entry, filepath, sizeof(filepath));

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

    ESP_LOGW(TAG, "RandomPick: Makapix - no artwork after 5 attempts (available=%zu)",
             ch->available_count);
    return pick_recency_makapix(state, channel_index, out_artwork);
}

/**
 * @brief Pick random artwork, dispatches based on entry format
 */
static bool pick_random(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];

    if (ch->entry_format == PS_ENTRY_FORMAT_SDCARD) {
        return pick_random_sdcard(state, channel_index, out_artwork);
    } else {
        return pick_random_makapix(state, channel_index, out_artwork);
    }
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

// ============================================================================
// Multi-Channel Pick (used by play_scheduler_next)
// ============================================================================

bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (!state || !out_artwork || state->channel_count == 0) {
        return false;
    }

    // Count active channels with available artwork
    size_t active_count = 0;
    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];
        if (!ch->active || ch->entry_count == 0) {
            continue;
        }
        // For Makapix channels, check available_count (LAi)
        // For SD card channels, check entry_count (no LAi yet)
        if (ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX) {
            if (ch->available_count > 0) {
                active_count++;
            }
        } else {
            // SD card - no LAi, count as active if has entries
            active_count++;
        }
    }

    if (active_count == 0) {
        ESP_LOGD(TAG, "No active channels with available artwork");
        return false;
    }

    // Try each active channel via SWRR
    for (size_t attempt = 0; attempt < active_count; attempt++) {
        int ch_idx = ps_swrr_select_channel(state);
        if (ch_idx < 0) {
            break;
        }

        if (ps_pick_artwork(state, (size_t)ch_idx, out_artwork)) {
            return true;
        }
        // Channel exhausted - SWRR will pick different one next iteration
    }

    ESP_LOGD(TAG, "No available artwork in any channel");
    return false;
}

bool ps_peek_next_available(const ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (!state || !out_artwork || state->channel_count == 0) {
        return false;
    }

    // Create a temporary copy of mutable state to avoid modifying original
    // We need to copy SWRR credits and channel cursors
    ps_state_t temp;
    memcpy(&temp, state, sizeof(ps_state_t));

    // Use the temp copy for picking
    return ps_pick_next_available(&temp, out_artwork);
}
