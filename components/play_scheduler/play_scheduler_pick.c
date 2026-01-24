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
#include "channel_cache.h"
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

static __attribute__((unused)) bool has_404_marker(const char *filepath)
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

uint32_t ps_prng_next(uint64_t *state)
{
    // PCG32 (Permuted Congruential Generator) - requires 64-bit state
    uint64_t oldstate = *state;
    *state = oldstate * 6364136223846793005ULL + 1;
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

void ps_prng_seed(uint64_t *state, uint64_t seed)
{
    *state = seed;
    ps_prng_next(state);  // Advance once to mix in the seed
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
        ESP_LOGW(TAG, "RecencyPick SD[%zu]: FAIL - no entries (entries=%p, count=%zu)",
                 channel_index, (void*)entries, ch->entry_count);
        return false;
    }

    ESP_LOGI(TAG, "RecencyPick SD[%zu] '%s': pool_size=%zu, start_cursor=%lu",
             channel_index, ch->channel_id, ch->entry_count, (unsigned long)ch->cursor);

    uint32_t start_cursor = ch->cursor;
    bool wrapped = false;
    int skipped_missing = 0;
    int skipped_repeat = 0;

    while (true) {
        if (ch->cursor >= ch->entry_count) {
            if (wrapped) break;
            ch->cursor = 0;
            wrapped = true;
            ESP_LOGD(TAG, "  RecencyPick SD: cursor wrapped to 0");
        }

        if (wrapped && ch->cursor >= start_cursor) break;

        uint32_t current_index = ch->cursor;
        sdcard_index_entry_t *entry = &entries[ch->cursor];
        ch->cursor++;

        // Build filepath for this entry
        char filepath[256];
        ps_build_sdcard_filepath(entry, filepath, sizeof(filepath));

        // AVAILABILITY MASKING: skip if file doesn't exist
        if (!file_exists(filepath)) {
            ESP_LOGD(TAG, "  RecencyPick SD: index[%lu] '%s' missing", (unsigned long)current_index, entry->filename);
            skipped_missing++;
            continue;
        }

        // Skip immediate repeat (but allow if only 1 entry in channel)
        if (entry->post_id == state->last_played_id && ch->entry_count > 1) {
            ESP_LOGD(TAG, "  RecencyPick SD: index[%lu] post_id=%ld skipped (repeat)",
                     (unsigned long)current_index, (long)entry->post_id);
            skipped_repeat++;
            continue;
        }

        ESP_LOGI(TAG, ">>> PICKED (RecencyPick SD): index=%lu, post_id=%ld, pool_size=%zu, "
                 "skipped_missing=%d, skipped_repeat=%d, file=%s",
                 (unsigned long)current_index, (long)entry->post_id, ch->entry_count,
                 skipped_missing, skipped_repeat, entry->filename);

        // Found valid artwork
        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        out_artwork->storage_key[0] = '\0';  // No storage key for local files
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = 0;  // Use global config default
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGW(TAG, "RecencyPick SD[%zu]: EXHAUSTED (entries=%zu, skipped_missing=%d, skipped_repeat=%d)",
             channel_index, ch->entry_count, skipped_missing, skipped_repeat);
    return false;
}

/**
 * @brief Pick artwork from Makapix channel using recency mode (LAi-based)
 *
 * Uses LAi (available_post_ids) for O(1) availability checking.
 * Iterates through locally available artworks only.
 * Accesses ch->cache->* directly since cache arrays may be reallocated during merges.
 */
static bool pick_recency_makapix(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];

    // Makapix channels must have a cache
    if (!ch->cache) {
        ESP_LOGW(TAG, "RecencyPick Makapix[%zu]: FAIL - no cache", channel_index);
        return false;
    }

    // Access cache directly to avoid stale alias pointers after merge
    makapix_channel_entry_t *entries = ch->cache->entries;
    size_t entry_count = ch->cache->entry_count;
    int32_t *available_post_ids = ch->cache->available_post_ids;
    size_t available_count = ch->cache->available_count;

    if (!entries || entry_count == 0) {
        ESP_LOGW(TAG, "RecencyPick Makapix[%zu]: FAIL - no entries", channel_index);
        return false;
    }

    // Use LAi for availability masking - no filesystem I/O
    if (!available_post_ids || available_count == 0) {
        ESP_LOGW(TAG, "RecencyPick Makapix[%zu] '%s': FAIL - Ci=%zu but LAi=0 (no downloaded files)",
                 channel_index, ch->channel_id, entry_count);
        return false;
    }

    ESP_LOGI(TAG, "RecencyPick Makapix[%zu] '%s': pool_size(LAi)=%zu, Ci=%zu, start_cursor=%lu",
             channel_index, ch->channel_id, available_count, entry_count, (unsigned long)ch->cursor);

    // Cursor operates over available_post_ids (LAi), not full Ci
    uint32_t start_cursor = ch->cursor;
    bool wrapped = false;
    int skipped_count = 0;

    while (true) {
        if (ch->cursor >= available_count) {
            if (wrapped) break;
            ch->cursor = 0;
            wrapped = true;
            ESP_LOGD(TAG, "  RecencyPick: cursor wrapped to 0");
        }

        if (wrapped && ch->cursor >= start_cursor) break;

        // Get post_id from LAi, then lookup Ci index
        uint32_t lai_index = ch->cursor;
        int32_t post_id = available_post_ids[ch->cursor];
        ch->cursor++;

        uint32_t ci_index = ci_find_by_post_id(ch->cache, post_id);
        if (ci_index == UINT32_MAX) {
            ESP_LOGW(TAG, "  RecencyPick: LAi[%lu] post_id=%ld NOT FOUND in Ci (hash miss)",
                     (unsigned long)lai_index, (long)post_id);
            skipped_count++;
            continue;
        }

        makapix_channel_entry_t *entry = &entries[ci_index];

        // Skip non-artwork entries (playlists, etc.)
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            ESP_LOGD(TAG, "  RecencyPick: LAi[%lu] post_id=%ld skipped (kind=%d, not artwork)",
                     (unsigned long)lai_index, (long)post_id, entry->kind);
            skipped_count++;
            continue;
        }

        // Skip immediate repeat (but allow if only 1 available entry)
        if (entry->post_id == state->last_played_id && available_count > 1) {
            ESP_LOGD(TAG, "  RecencyPick: LAi[%lu] post_id=%ld skipped (repeat of last_played)",
                     (unsigned long)lai_index, (long)post_id);
            skipped_count++;
            continue;
        }

        // Build filepath for this entry
        char filepath[256];
        ps_build_vault_filepath(entry, filepath, sizeof(filepath));

        // Found valid artwork - build storage_key string
        char storage_key[40];
        bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

        ESP_LOGI(TAG, ">>> PICKED (RecencyPick Makapix): LAi_index=%lu, Ci_index=%lu, post_id=%ld, "
                 "pool_size=%zu, skipped=%d, storage_key=%.8s...",
                 (unsigned long)lai_index, (unsigned long)ci_index, (long)post_id,
                 available_count, skipped_count, storage_key);

        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        strlcpy(out_artwork->storage_key, storage_key, sizeof(out_artwork->storage_key));
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = 0;  // Use global config default
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGW(TAG, "RecencyPick Makapix[%zu]: EXHAUSTED after scanning %zu entries (skipped=%d)",
             channel_index, available_count, skipped_count);
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
        ESP_LOGW(TAG, "RandomPick SD[%zu]: FAIL - no entries", channel_index);
        return false;
    }

    ESP_LOGI(TAG, "RandomPick SD[%zu] '%s': pool_size=%zu",
             channel_index, ch->channel_id, ch->entry_count);

    // Sample from all entries for true shuffle
    size_t r_eff = ch->entry_count;

    for (int attempt = 0; attempt < 5; attempt++) {
        uint32_t r = ps_prng_next(&ch->pick_rng_state);
        size_t index = (size_t)(r % r_eff);

        sdcard_index_entry_t *entry = &entries[index];

        char filepath[256];
        ps_build_sdcard_filepath(entry, filepath, sizeof(filepath));

        ESP_LOGD(TAG, "  RandomPick SD attempt %d: r=%lu, index=%zu (mod %zu), file=%s",
                 attempt + 1, (unsigned long)r, index, r_eff, entry->filename);

        if (!file_exists(filepath)) {
            ESP_LOGD(TAG, "  RandomPick SD: index[%zu] file missing", index);
            continue;
        }

        // Skip immediate repeat (but allow if only 1 entry or after several attempts)
        if (entry->post_id == state->last_played_id && ch->entry_count > 1 && attempt < 4) {
            ESP_LOGD(TAG, "  RandomPick SD: index[%zu] post_id=%ld skipped (repeat)", index, (long)entry->post_id);
            continue;
        }

        ESP_LOGI(TAG, ">>> PICKED (RandomPick SD): index=%zu, post_id=%ld, pool_size=%zu, attempt=%d, file=%s",
                 index, (long)entry->post_id, ch->entry_count, attempt + 1, entry->filename);

        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        out_artwork->storage_key[0] = '\0';
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = 0;  // Use global config default
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGW(TAG, "RandomPick SD[%zu]: FAILED after 5 attempts, falling back to RecencyPick", channel_index);
    return pick_recency_sdcard(state, channel_index, out_artwork);
}

/**
 * @brief Pick random artwork from Makapix channel (LAi-based)
 *
 * Uses LAi (available_post_ids) for O(1) random picks.
 * Samples directly from locally available artworks.
 * Accesses ch->cache->* directly since cache arrays may be reallocated during merges.
 */
static bool pick_random_makapix(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork)
{
    ps_channel_state_t *ch = &state->channels[channel_index];

    // Makapix channels must have a cache
    if (!ch->cache) {
        ESP_LOGW(TAG, "RandomPick Makapix[%zu]: FAIL - no cache", channel_index);
        return false;
    }

    // Access cache directly to avoid stale alias pointers after merge
    makapix_channel_entry_t *entries = ch->cache->entries;
    size_t entry_count = ch->cache->entry_count;
    int32_t *available_post_ids = ch->cache->available_post_ids;
    size_t available_count = ch->cache->available_count;

    if (!entries || entry_count == 0) {
        ESP_LOGW(TAG, "RandomPick Makapix[%zu]: FAIL - no entries", channel_index);
        return false;
    }

    // Use LAi for O(1) random picks - no filesystem I/O
    if (!available_post_ids || available_count == 0) {
        ESP_LOGW(TAG, "RandomPick Makapix[%zu] '%s': FAIL - Ci=%zu but LAi=0 (no downloaded files)",
                 channel_index, ch->channel_id, entry_count);
        return false;
    }

    ESP_LOGI(TAG, "RandomPick Makapix[%zu] '%s': pool_size(LAi)=%zu, Ci=%zu",
             channel_index, ch->channel_id, available_count, entry_count);

    // Sample from available_post_ids (LAi) directly
    for (int attempt = 0; attempt < 5; attempt++) {
        uint32_t r = ps_prng_next(&ch->pick_rng_state);
        size_t lai_index = (size_t)(r % available_count);
        int32_t post_id = available_post_ids[lai_index];

        ESP_LOGD(TAG, "  RandomPick attempt %d: r=%lu, LAi_index=%zu (mod %zu), post_id=%ld",
                 attempt + 1, (unsigned long)r, lai_index, available_count, (long)post_id);

        uint32_t ci_index = ci_find_by_post_id(ch->cache, post_id);
        if (ci_index == UINT32_MAX) {
            ESP_LOGW(TAG, "  RandomPick: LAi[%zu] post_id=%ld NOT FOUND in Ci", lai_index, (long)post_id);
            continue;
        }

        makapix_channel_entry_t *entry = &entries[ci_index];

        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            ESP_LOGD(TAG, "  RandomPick: LAi[%zu] post_id=%ld skipped (kind=%d)", lai_index, (long)post_id, entry->kind);
            continue;
        }

        // Skip immediate repeat (but allow if only 1 available or after several attempts)
        if (entry->post_id == state->last_played_id && available_count > 1 && attempt < 4) {
            ESP_LOGD(TAG, "  RandomPick: LAi[%zu] post_id=%ld skipped (repeat)", lai_index, (long)post_id);
            continue;
        }

        char filepath[256];
        ps_build_vault_filepath(entry, filepath, sizeof(filepath));

        char storage_key[40];
        bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

        ESP_LOGI(TAG, ">>> PICKED (RandomPick Makapix): LAi_index=%zu, Ci_index=%lu, post_id=%ld, "
                 "pool_size=%zu, attempt=%d, storage_key=%.8s...",
                 lai_index, (unsigned long)ci_index, (long)post_id,
                 available_count, attempt + 1, storage_key);

        out_artwork->artwork_id = entry->post_id;
        out_artwork->post_id = entry->post_id;
        strlcpy(out_artwork->filepath, filepath, sizeof(out_artwork->filepath));
        strlcpy(out_artwork->storage_key, storage_key, sizeof(out_artwork->storage_key));
        out_artwork->created_at = entry->created_at;
        out_artwork->dwell_time_ms = 0;  // Use global config default
        out_artwork->type = get_asset_type_from_extension(entry->extension);
        out_artwork->channel_index = (uint8_t)channel_index;

        return true;
    }

    ESP_LOGW(TAG, "RandomPick Makapix[%zu]: FAILED after 5 attempts (available=%zu), falling back to RecencyPick",
             channel_index, available_count);
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

    // Count active channels with available artwork and log pool sizes
    size_t active_count = 0;
    size_t total_ci = 0;
    size_t total_lai = 0;

    ESP_LOGI(TAG, "=== PICK DEBUG: ps_pick_next_available() ===");
    ESP_LOGI(TAG, "Pick mode: %s, Exposure mode: %d, Channels: %zu",
             state->pick_mode == PS_PICK_RANDOM ? "RANDOM" : "RECENCY",
             state->exposure_mode, state->channel_count);

    for (size_t i = 0; i < state->channel_count; i++) {
        ps_channel_state_t *ch = &state->channels[i];

        if (ch->entry_format == PS_ENTRY_FORMAT_MAKAPIX && ch->cache) {
            size_t ci_count = ch->cache->entry_count;
            size_t lai_count = ch->cache->available_count;
            ESP_LOGI(TAG, "  Ch[%zu] '%s': Ci=%zu, LAi=%zu, cursor=%lu, active=%d, weight=%lu",
                     i, ch->channel_id, ci_count, lai_count,
                     (unsigned long)ch->cursor, ch->active, (unsigned long)ch->weight);
            total_ci += ci_count;
            total_lai += lai_count;

            if (!ch->active || ci_count == 0) {
                continue;
            }
            if (lai_count > 0) {
                active_count++;
            }
        } else {
            // SD card channel or Makapix without cache
            ESP_LOGI(TAG, "  Ch[%zu] '%s' (SD): entries=%zu, cursor=%lu, active=%d, weight=%lu",
                     i, ch->channel_id, ch->entry_count,
                     (unsigned long)ch->cursor, ch->active, (unsigned long)ch->weight);
            total_ci += ch->entry_count;
            total_lai += ch->entry_count;  // SD card has no LAi distinction

            if (!ch->active || ch->entry_count == 0) {
                continue;
            }
            // SD card - no LAi, count as active if has entries
            active_count++;
        }
    }

    ESP_LOGI(TAG, "TOTALS: active_channels=%zu, total_Ci=%zu, total_LAi=%zu",
             active_count, total_ci, total_lai);

    if (active_count == 0) {
        ESP_LOGW(TAG, "PICK FAILED: No active channels with available artwork");
        return false;
    }

    // Try each active channel via SWRR
    for (size_t attempt = 0; attempt < active_count; attempt++) {
        int ch_idx = ps_swrr_select_channel(state);
        if (ch_idx < 0) {
            ESP_LOGW(TAG, "SWRR returned -1 on attempt %zu", attempt);
            break;
        }

        ESP_LOGI(TAG, "SWRR selected channel[%d] '%s' (attempt %zu/%zu)",
                 ch_idx, state->channels[ch_idx].channel_id, attempt + 1, active_count);

        if (ps_pick_artwork(state, (size_t)ch_idx, out_artwork)) {
            return true;
        }
        ESP_LOGW(TAG, "Channel[%d] exhausted, trying next", ch_idx);
        // Channel exhausted - SWRR will pick different one next iteration
    }

    ESP_LOGW(TAG, "PICK FAILED: No available artwork in any channel after %zu attempts", active_count);
    return false;
}

/**
 * @brief Save mutable pick state to snapshot
 *
 * Saves only the fields that ps_pick_next_available() might modify.
 * This is ~1KB instead of copying the entire ps_state_t (~21KB).
 */
static void ps_save_pick_state(const ps_state_t *state, ps_pick_snapshot_t *snapshot)
{
    for (size_t i = 0; i < state->channel_count && i < PS_MAX_CHANNELS; i++) {
        snapshot->channels[i].credit = state->channels[i].credit;
        snapshot->channels[i].cursor = state->channels[i].cursor;
        snapshot->channels[i].pick_rng_state = state->channels[i].pick_rng_state;
    }
    snapshot->epoch_id = state->epoch_id;
    snapshot->last_played_id = state->last_played_id;
}

/**
 * @brief Restore mutable pick state from snapshot
 */
static void ps_restore_pick_state(ps_state_t *state, const ps_pick_snapshot_t *snapshot)
{
    for (size_t i = 0; i < state->channel_count && i < PS_MAX_CHANNELS; i++) {
        state->channels[i].credit = snapshot->channels[i].credit;
        state->channels[i].cursor = snapshot->channels[i].cursor;
        state->channels[i].pick_rng_state = snapshot->channels[i].pick_rng_state;
    }
    state->epoch_id = snapshot->epoch_id;
    state->last_played_id = snapshot->last_played_id;
}

bool ps_peek_next_available(ps_state_t *state, ps_artwork_t *out_artwork)
{
    if (!state || !out_artwork || state->channel_count == 0) {
        return false;
    }

    // Save mutable pick state to lightweight snapshot (~1KB vs ~21KB for full copy)
    ps_pick_snapshot_t snapshot;
    ps_save_pick_state(state, &snapshot);

    // Perform pick (modifies state)
    bool result = ps_pick_next_available(state, out_artwork);

    // Restore state so it appears unchanged to caller
    ps_restore_pick_state(state, &snapshot);

    return result;
}
