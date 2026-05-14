// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file play_scheduler_playsets.c
 * @brief Play Scheduler - Playset execution and cache loading
 *
 * This file implements playset execution including:
 * - Channel cache loading (SD card and Makapix formats)
 * - execute_playset() for multi-channel playset setup
 * - Convenience functions for named/user/hashtag channels
 *
 * See play_scheduler_types.h for the playset type definition.
 */

#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "view_tracker.h"
#include "p3a_state.h"
#include "p3a_current_post.h"
#include "sd_path.h"
#include "content_cache.h"
#include "makapix.h"
#include "giphy.h"
#include "pin_lists.h"
#include "psram_alloc.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Forward declaration for gBool type (from uGFX)
typedef int gBool;

// UI dismiss helpers (from app_lcd.c / ugfx_ui.c via weak symbols)
// Used to auto-dismiss info screen when a playset is activated by the user
extern bool app_lcd_is_ui_mode(void) __attribute__((weak));
extern esp_err_t app_lcd_exit_ui_mode(void) __attribute__((weak));
extern void ugfx_ui_hide_info_screen(void) __attribute__((weak));
extern gBool ugfx_ui_is_active(void) __attribute__((weak));

static const char *TAG = "ps_commands";

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Compute channel_id as first 16 hex chars of SHA256("{type}:{name}:{identifier}")
 *
 * Produces an opaque, filesystem-safe identifier. The resulting 16-char hex
 * string fits comfortably in existing char channel_id[64] buffers.
 */
void ps_compute_channel_id(ps_channel_type_t type, const char *name,
                           const char *identifier, char *out_id, size_t max_len)
{
    if (!out_id || max_len == 0) return;
    if (!name) name = "";
    if (!identifier) identifier = "";

    // Build canonical string "{type_int}:{name}:{identifier}"
    char canonical[256];
    int len = snprintf(canonical, sizeof(canonical), "%d:%s:%s", (int)type, name, identifier);
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(canonical)) len = sizeof(canonical) - 1;

    // Compute SHA256
    uint8_t sha256[32];
    if (mbedtls_sha256((const unsigned char *)canonical, (size_t)len, sha256, 0) != 0) {
        // Fallback: use truncated canonical string
        strlcpy(out_id, canonical, max_len);
        return;
    }

    // Format first 8 bytes as 16 hex chars
    size_t hex_len = 16;
    if (hex_len >= max_len) hex_len = max_len - 1;
    for (size_t i = 0; i < hex_len / 2 && i < 8; i++) {
        snprintf(out_id + i * 2, max_len - i * 2, "%02x", sha256[i]);
    }
    out_id[hex_len] = '\0';
}

void ps_ensure_display_name(ps_channel_spec_t *spec)
{
    if (spec->display_name[0] != '\0') return;

    // Delegate to the canonical display name builder
    ps_get_display_name_from_spec(spec->type, spec->name, spec->identifier,
                                  spec->display_name, sizeof(spec->display_name));
}

// ps_build_channel_id() removed — replaced by ps_compute_channel_id() above

// ============================================================================
// Cache Path Building
// ============================================================================

void ps_build_cache_path(const char *channel_id, char *out_path, size_t max_len)
{
    char channel_dir[256];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        strlcpy(channel_dir, "/sdcard/p3a/channel", sizeof(channel_dir));
    }

    // channel_id is a hex hash — always filesystem-safe, no sanitization needed
    snprintf(out_path, max_len, "%s/%s.bin", channel_dir, channel_id);
}

// ============================================================================
// Cache Loading
// ============================================================================

/**
 * @brief Load SD card channel cache (raw binary format)
 */
static esp_err_t ps_load_sdcard_cache(ps_channel_state_t *ch)
{
    char cache_path[512];
    char channel_dir[256];
    if (sd_path_get_channel(channel_dir, sizeof(channel_dir)) != ESP_OK) {
        strlcpy(channel_dir, "/sdcard/p3a/channel", sizeof(channel_dir));
    }
    snprintf(cache_path, sizeof(cache_path), "%s/sdcard.bin", channel_dir);

    struct stat st;
    if (stat(cache_path, &st) != 0) {
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        ESP_LOGD(TAG, "Channel '%s': no cache file", ch->display_name);
        return ESP_ERR_NOT_FOUND;
    }

    size_t entry_size = sizeof(sdcard_index_entry_t);  // 160 bytes

    // A 0-byte file is the legitimate "we scanned, found no artworks" marker
    // written by ps_build_sdcard_index when /<root>/animations/ is empty.
    // Treat it as a valid empty cache so the SD card channel can advertise
    // the empty state to the rest of the scheduler.
    if (st.st_size == 0) {
        if (ch->entries) {
            free(ch->entries);
            ch->entries = NULL;
        }
        ch->cache_loaded = true;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_SDCARD;
        ESP_LOGI(TAG, "Channel '%s': empty SD card cache (no artworks)", ch->display_name);
        return ESP_OK;
    }

    if (st.st_size < 0 || st.st_size % entry_size != 0) {
        ESP_LOGW(TAG, "Channel '%s': invalid cache file size %ld (expected multiple of %zu)",
                 ch->display_name, (long)st.st_size, entry_size);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_ERR_INVALID_SIZE;
    }

    ch->entry_count = st.st_size / entry_size;

    if (ch->entries) {
        free(ch->entries);
        ch->entries = NULL;
    }

    ch->entries = psram_malloc(ch->entry_count * entry_size);
    if (!ch->entries) {
        ESP_LOGE(TAG, "Channel '%s': failed to allocate %zu entries", ch->display_name, ch->entry_count);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(cache_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Channel '%s': failed to open cache file", ch->display_name);
        free(ch->entries);
        ch->entries = NULL;
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_FAIL;
    }

    size_t read_count = fread(ch->entries, entry_size, ch->entry_count, f);
    fclose(f);

    if (read_count != ch->entry_count) {
        ESP_LOGE(TAG, "Channel '%s': read %zu/%zu entries", ch->display_name, read_count, ch->entry_count);
        free(ch->entries);
        ch->entries = NULL;
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_FAIL;
    }

    // SD-card channels have no LAi suppression — every entry on disk is always
    // playable, so available_count tracks entry_count one-for-one. Keeping the
    // field populated here lets play_scheduler_get_total_available() (and any
    // other consumer that reads ch->available_count when ch->cache is NULL)
    // see the true count instead of zero.
    ch->available_count = ch->entry_count;
    ch->cache_loaded = true;
    ch->active = (ch->entry_count > 0);
    ch->entry_format = PS_ENTRY_FORMAT_SDCARD;

    ps_touch_cache_file(ch->channel_id, PS_CHANNEL_TYPE_SDCARD);

    ESP_LOGI(TAG, "Channel '%s': loaded cache with %zu entries (sdcard format) into memory",
             ch->display_name, ch->entry_count);

    return ESP_OK;
}

/**
 * @brief Load Makapix channel cache using channel_cache module
 *
 * Uses the unified cache file format with LAi persistence. Cache files written
 * by older firmware versions are rejected at load time and a fresh empty cache
 * is started; the next refresh + download cycle repopulates Ci and LAi.
 */
static esp_err_t ps_load_makapix_cache(ps_channel_state_t *ch)
{
    // Get paths
    char channels_path[128];
    char vault_path[128];
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        strlcpy(channels_path, "/sdcard/p3a/channel", sizeof(channels_path));
    }
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        strlcpy(vault_path, "/sdcard/p3a/vault", sizeof(vault_path));
    }

    // Free existing cache if any (handles channel switch). Hold the cache
    // lifecycle lock so concurrent readers can't be mid-use on the cache.
    if (ch->cache) {
        channel_cache_lifecycle_lock();
        channel_cache_unregister(ch->cache);
        channel_cache_free(ch->cache);
        free(ch->cache);
        ch->cache = NULL;
        ch->entries = NULL;
        ch->available_post_ids = NULL;
        channel_cache_lifecycle_unlock();
    }

    // Allocate new cache structure
    ch->cache = malloc(sizeof(channel_cache_t));
    if (!ch->cache) {
        ESP_LOGE(TAG, "Channel '%s': failed to allocate cache structure", ch->display_name);
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return ESP_ERR_NO_MEM;
    }

    // Load cache (handles legacy format migration, CRC validation, LAi persistence)
    esp_err_t err = channel_cache_load(ch->channel_id, (uint8_t)ch->type, ch->display_name, channels_path, vault_path, ch->cache);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel '%s': channel_cache_load failed: %s",
                 ch->display_name, esp_err_to_name(err));
        free(ch->cache);
        ch->cache = NULL;
        ch->cache_loaded = false;
        ch->entry_count = 0;
        ch->available_count = 0;
        ch->active = false;
        ch->weight = 0;
        ch->entry_format = PS_ENTRY_FORMAT_NONE;
        return err;
    }

    // Register for debounced persistence
    esp_err_t reg_err = channel_cache_register(ch->cache);
    if (reg_err != ESP_OK) {
        ESP_LOGW(TAG, "Channel '%s': cache register failed: %s (persistence disabled)",
                 ch->display_name, esp_err_to_name(reg_err));
    }

    // Belt-and-suspenders: if the load path left the cache dirty (e.g.
    // cache_trim_to_cap shrank it to a lowered soft cap), make sure a save
    // is scheduled. Legacy-format migration no longer runs — old versions
    // are rejected outright at validation — so trim is currently the only
    // cause, but any future load-side mutation will also benefit from this.
    if (ch->cache->dirty) {
        ESP_LOGI(TAG, "Channel '%s': cache dirty after load, scheduling save", ch->display_name);
        channel_cache_schedule_save(ch->cache);
    }

    // For Makapix channels, all code accesses ch->cache->* directly to avoid stale
    // pointers when cache arrays are reallocated during batch merges.
    ch->entries = NULL;
    ch->entry_count = 0;
    ch->available_post_ids = NULL;
    ch->available_count = 0;

    ch->cache_loaded = true;
    ch->active = (ch->cache->available_count > 0);
    ch->entry_format = PS_ENTRY_FORMAT_MAKAPIX;

    ps_touch_cache_file(ch->channel_id, ch->type);

    ESP_LOGI(TAG, "Channel '%s': loaded cache with %zu entries, %zu available (makapix format)",
             ch->display_name, ch->cache->entry_count, ch->cache->available_count);

    return ESP_OK;
}

/**
 * @brief Load cache file for a channel
 *
 * Loads .bin file if it exists and sets entry_count and active flag.
 * Channels without cache get weight=0 until refresh completes.
 *
 * SD card channels use sdcard_index_entry_t (160 bytes per entry).
 * Makapix channels use makapix_channel_entry_t (64 bytes per entry).
 */
esp_err_t ps_load_channel_cache(ps_channel_state_t *ch)
{
    // Artwork channels are in-memory only - no cache file
    if (ch->type == PS_CHANNEL_TYPE_ARTWORK) {
        ch->cache_loaded = true;  // Mark as "loaded" (single entry from spec)
        ch->entry_count = 1;
        ch->active = false;  // Will become active after download check/completion
        ch->entry_format = PS_ENTRY_FORMAT_NONE;  // No standard entry format
        return ESP_OK;
    }

    // SD card channels use raw binary format (no LAi needed - files are always local)
    if (ch->type == PS_CHANNEL_TYPE_SDCARD) {
        return ps_load_sdcard_cache(ch);
    }

    // Giphy channels use the same channel_cache module as Makapix (64-byte
    // entries); only the entry_format tag differs so the picker/download
    // manager can dispatch on giphy/ vs vault/ paths.
    if (ch->type == PS_CHANNEL_TYPE_GIPHY) {
        esp_err_t err = ps_load_makapix_cache(ch);
        if (err == ESP_OK) {
            ch->entry_format = PS_ENTRY_FORMAT_GIPHY;
        }
        return err;
    }

    // Institution (museum) channels also share the 64-byte channel_cache slot
    // with Makapix and Giphy; institution_channel_entry_t is byte-compatible.
    // The entry_format tag routes the picker/download manager through the
    // museum/-rooted vault and the IIIF URL builders.
    if (ch->type == PS_CHANNEL_TYPE_INSTITUTION) {
        esp_err_t err = ps_load_makapix_cache(ch);
        if (err == ESP_OK) {
            ch->entry_format = PS_ENTRY_FORMAT_INSTITUTION;
        }
        return err;
    }

    // Pinned channels: the list of pins lives entirely in pin_lists' order.bin.
    // We load it into the SD-card-style ch->entries slot (heap buffer of
    // pinned_order_entry_t) since pinned channels have no remote backing and
    // therefore no LAi cycle.
    if (ch->type == PS_CHANNEL_TYPE_PINNED) {
        pinned_order_entry_t *entries = NULL;
        size_t count = 0;
        esp_err_t err = pin_lists_channel_load(ch->identifier, &entries, &count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Pinned channel '%s': load failed: %s",
                     ch->identifier, esp_err_to_name(err));
            return err;
        }
        ch->entries = entries;
        ch->entry_count = count;
        ch->available_post_ids = NULL;
        ch->available_count = 0;
        ch->cache = NULL;
        ch->cache_loaded = true;
        ch->active = (count > 0);
        ch->entry_format = PS_ENTRY_FORMAT_PINNED;
        ESP_LOGI(TAG, "Pinned channel '%s': loaded %zu entries", ch->identifier, count);
        return ESP_OK;
    }

    // Makapix channels use channel_cache module for LAi persistence
    return ps_load_makapix_cache(ch);
}

// ============================================================================
// Command Execution
// ============================================================================

esp_err_t play_scheduler_execute_playset(const ps_playset_t *playset)
{
    ps_state_t *s_state = ps_get_state();

    if (!s_state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!playset || playset->channel_count == 0 || playset->channel_count > PS_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    // Cancel all active Makapix refresh tasks before setting up new channels
    // This prevents old refresh tasks from wasting MQTT queries when switching channels
    makapix_cancel_all_refreshes();
    giphy_cancel_refresh();

    // Reset the periodic refresh timer so this playset triggers immediate refresh
    ps_refresh_reset_timer();

    // Stop view tracking for the old channel before switching
    // This prevents view events from being sent for the wrong channel
    view_tracker_stop();

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Executing playset: %zu channel(s), pick=%d (global)",
             playset->channel_count, s_state->pick_mode);

    // Free old channel entries before reconfiguring. Hold the cache
    // lifecycle lock so concurrent readers (e.g. download_task) cannot be
    // mid-iteration on a cache that we're about to free.
    channel_cache_lifecycle_lock();
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        if (ch->cache) {
            // Makapix channel - cache owns all memory
            channel_cache_unregister(ch->cache);
            channel_cache_free(ch->cache);
            free(ch->cache);
            ch->cache = NULL;
            ch->entries = NULL;
            ch->available_post_ids = NULL;
            ch->available_count = 0;
        } else if (ch->entries) {
            // SD card channel - entries owned directly
            free(ch->entries);
            ch->entries = NULL;
        }
    }
    channel_cache_lifecycle_unlock();

    // Store playset parameters (pick_mode is global; not stored per playset)
    s_state->channel_count = playset->channel_count;

    // Increment epoch (history is preserved)
    s_state->epoch_id++;

    // Reset history navigation pointer so the new playset's first pick
    // becomes the new CURRENT. Without this, if the user had navigated
    // back N steps before switching playsets, play_scheduler_next() would
    // see history_position >= 0 and walk forward one slot through stale
    // history instead of picking fresh from the newly loaded channels.
    s_state->history_position = -1;

    // Resolve channel sidecar directory once for last_refresh hydration below
    char ch_meta_path[128];
    if (sd_path_get_channel(ch_meta_path, sizeof(ch_meta_path)) != ESP_OK) {
        strlcpy(ch_meta_path, "/sdcard/p3a/channel", sizeof(ch_meta_path));
    }

    // Initialize each channel
    for (size_t i = 0; i < playset->channel_count; i++) {
        const ps_channel_spec_t *spec = &playset->channels[i];
        ps_channel_state_t *ch = &s_state->channels[i];

        // Build channel_id as hash of spec and preserve original fields
        ps_compute_channel_id(spec->type, spec->name, spec->identifier,
                              ch->channel_id, sizeof(ch->channel_id));
        strlcpy(ch->identifier, spec->identifier, sizeof(ch->identifier));
        ch->type = spec->type;
        strlcpy(ch->spec_name, spec->name, sizeof(ch->spec_name));
        if (spec->display_name[0] != '\0') {
            strlcpy(ch->display_name, spec->display_name, sizeof(ch->display_name));
        } else {
            ps_get_display_name_from_spec(spec->type, spec->name, spec->identifier,
                                          ch->display_name, sizeof(ch->display_name));
        }

        // Reset SWRR state
        ch->credit = 0;
        ch->weight = spec->weight;  // Will be recalculated after cache load
        ch->spec_weight = spec->weight;  // Preserve original for weight recalculation

        // Reset pick state
        ch->cursor = 0;
        ps_prng_seed(&ch->pick_rng_state, s_state->global_seed ^ (uint32_t)i ^ s_state->epoch_id);

        // Clear legacy handle
        ch->handle = NULL;

        // Reset refresh state
        ch->refresh_pending = true;  // Queue for background refresh
        ch->refresh_in_progress = false;
        ch->refresh_async_pending = false;
        ch->refresh_start_tick = 0;

        // Hydrate last_refresh from on-disk sidecar so the picker can rank
        // channels by oldest-first across reboots and partial refresh cycles.
        // SD card / artwork channels have no sidecar — they default to 0,
        // which makes them refresh first (cheap operations, no quota concern).
        ch->last_refresh = 0;
        channel_metadata_t meta_init;
        if (channel_metadata_load(ch->channel_id, ch_meta_path, &meta_init) == ESP_OK) {
            ch->last_refresh = meta_init.last_refresh;
        }

        // Initialize artwork-specific state if this is an artwork channel
        if (spec->type == PS_CHANNEL_TYPE_ARTWORK) {
            ch->artwork_state.post_id = spec->artwork.post_id;
            strlcpy(ch->artwork_state.storage_key, spec->artwork.storage_key, sizeof(ch->artwork_state.storage_key));
            strlcpy(ch->artwork_state.art_url, spec->artwork.art_url, sizeof(ch->artwork_state.art_url));
            strlcpy(ch->artwork_state.filepath, spec->artwork.filepath, sizeof(ch->artwork_state.filepath));
            ch->artwork_state.download_pending = true;  // Will check/download in refresh
            ch->artwork_state.download_in_progress = false;
            ch->refresh_pending = true;  // Trigger refresh to check/download
        }

        // Load existing cache if available
        ps_load_channel_cache(ch);

        size_t entries_count = (ch->cache ? ch->cache->entry_count : ch->entry_count);
        ESP_LOGD(TAG, "Channel[%zu]: '%s', type=%d, weight=%lu, active=%d, entries=%zu",
                 i, ch->display_name, ch->type, (unsigned long)ch->weight,
                 ch->active, entries_count);
    }

    // Calculate SWRR weights
    ps_swrr_calculate_weights(s_state);

    // Store first channel as "current" for status display
    if (playset->channel_count > 0) {
        strlcpy(s_state->current_channel_id, s_state->channels[0].channel_id,
                sizeof(s_state->current_channel_id));
    }

    // Signal background refresh task to process pending channels
    ps_refresh_signal_work();

    // Update content cache with new channel list for round-robin downloading
    const char *channel_ids[PS_MAX_CHANNELS];
    for (size_t i = 0; i < playset->channel_count; i++) {
        channel_ids[i] = s_state->channels[i].channel_id;
    }
    content_cache_set_channels(channel_ids, playset->channel_count);

    // Reset playback_initiated so cache can trigger playback for new channel
    content_cache_reset_playback_initiated();

    // Check if any channel has entries we can play immediately
    bool has_entries = false;
    char first_channel_display_name[64] = "Channel";
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        size_t entry_count = (ch->cache ? ch->cache->entry_count : ch->entry_count);
        if (ch->active && entry_count > 0) {
            has_entries = true;
            break;
        }
    }
    // Get first channel's display name for UI
    if (s_state->channel_count > 0) {
        ps_channel_state_t *ch0 = &s_state->channels[0];
        ps_get_display_name_from_spec(ch0->type, ch0->spec_name, ch0->identifier,
                                      first_channel_display_name, sizeof(first_channel_display_name));
    }

    s_state->playback_triggered = false;

    xSemaphoreGive(s_state->mutex);

    // Reset auto-swap timer so the full dwell interval starts from this new playset
    ps_timer_reset(s_state);

    // Dismiss info-screen overlay if active, so playback is visible immediately.
    // Safe to call at boot (app_lcd_is_ui_mode returns false when no UI is shown).
    if (app_lcd_is_ui_mode && app_lcd_is_ui_mode()) {
        if (ugfx_ui_hide_info_screen) ugfx_ui_hide_info_screen();
        // Only exit UI render mode if no other overlay remains active
        if (!ugfx_ui_is_active || !ugfx_ui_is_active()) {
            if (app_lcd_exit_ui_mode) app_lcd_exit_ui_mode();
        }
    }

    // Only trigger initial playback if we have entries
    // Otherwise, let download manager trigger it when first file is available
    if (has_entries) {
        esp_err_t next_err = play_scheduler_next(NULL);
        s_state->playback_triggered = true;
        return next_err;
    } else {
        // Distinguish "empty pinned list" (terminal — nothing will load) from
        // "remote channel not yet fetched" (transient — loading message).
        bool pinned_empty = (s_state->channel_count == 1 &&
                             s_state->channels[0].type == PS_CHANNEL_TYPE_PINNED);

        if (pinned_empty) {
            ESP_LOGI(TAG, "Pinned channel is empty - showing empty-state message");
        } else {
            ESP_LOGI(TAG, "No cached entries yet - waiting for refresh/download");
        }

        // Clear "currently playing" state so the web UI's /playsets/active
        // poll stops returning the previous channel's last artwork.
        // play_scheduler_current() reads from history, so we drop both the
        // history buffer and the on-screen post identity. For non-empty
        // channels this is unnecessary — play_scheduler_next() repopulates
        // history before returning — but those don't take this branch.
        xSemaphoreTake(s_state->mutex, portMAX_DELAY);
        ps_history_clear(s_state);
        xSemaphoreGive(s_state->mutex);
        p3a_current_post_clear();

        // Invalidate the old animation's front buffer so it doesn't show through
        // while we wait for the new channel to load. The channel message UI will
        // cover the display, so there's no visual glitch.
        extern void animation_player_invalidate(void);
        animation_player_invalidate();

        extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                   int progress_percent, const char *detail);
        if (pinned_empty) {
            p3a_render_set_channel_message(first_channel_display_name, 4 /* P3A_CHANNEL_MSG_EMPTY */, -1,
                                            "No artworks pinned to this list.\n"
                                            "Add pins to see them here.");
        } else if (p3a_state_has_wifi()) {
            // Show loading state to user while waiting for refresh/download
            // But only if we have WiFi connectivity (no point showing loading in AP mode)
            p3a_render_set_channel_message(first_channel_display_name, 1 /* P3A_CHANNEL_MSG_LOADING */, -1,
                                            "Loading channel...");
        }
        return ESP_OK;
    }
}

// ============================================================================
// Built-in Playset Creation
// ============================================================================

esp_err_t ps_create_channel_playset(const char *playset_name, ps_playset_t *out_playset)
{
    if (!playset_name || !out_playset) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_playset, 0, sizeof(*out_playset));
    out_playset->channel_count = 1;

    if (strcmp(playset_name, "channel_recent") == 0) {
        out_playset->channels[0].type = PS_CHANNEL_TYPE_NAMED;
        strlcpy(out_playset->channels[0].name, "all", sizeof(out_playset->channels[0].name));
        out_playset->channels[0].weight = 1;
    } else if (strcmp(playset_name, "channel_promoted") == 0) {
        out_playset->channels[0].type = PS_CHANNEL_TYPE_NAMED;
        strlcpy(out_playset->channels[0].name, "promoted", sizeof(out_playset->channels[0].name));
        out_playset->channels[0].weight = 1;
    } else if (strcmp(playset_name, "channel_sdcard") == 0) {
        out_playset->channels[0].type = PS_CHANNEL_TYPE_SDCARD;
        strlcpy(out_playset->channels[0].name, "sdcard", sizeof(out_playset->channels[0].name));
        out_playset->channels[0].weight = 1;
    } else if (strcmp(playset_name, "giphy_trending") == 0) {
        out_playset->channels[0].type = PS_CHANNEL_TYPE_GIPHY;
        strlcpy(out_playset->channels[0].name, "trending", sizeof(out_playset->channels[0].name));
        out_playset->channels[0].weight = 1;
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    ps_ensure_display_name(&out_playset->channels[0]);
    return ESP_OK;
}

// ============================================================================
// Convenience Functions
// ============================================================================

esp_err_t play_scheduler_play_named_channel(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_named_channel: %s", name);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Failed to allocate playset struct");
        return ESP_ERR_NO_MEM;
    }

    playset->channel_count = 1;

    // Determine channel type
    if (strcmp(name, "sdcard") == 0) {
        playset->channels[0].type = PS_CHANNEL_TYPE_SDCARD;
        strlcpy(playset->channels[0].name, "sdcard", sizeof(playset->channels[0].name));
    } else {
        playset->channels[0].type = PS_CHANNEL_TYPE_NAMED;
        strlcpy(playset->channels[0].name, name, sizeof(playset->channels[0].name));
    }
    playset->channels[0].weight = 1;
    ps_ensure_display_name(&playset->channels[0]);

    esp_err_t result = play_scheduler_execute_playset(playset);
    free(playset);
    return result;
}

esp_err_t play_scheduler_play_pinned_channel(const char *slug)
{
    /* NULL/empty slug → active list; pin_lists_channel_load handles the
       fallback internally. We still need a non-null identifier so the
       channel state has something to render in logs. */
    char target[PIN_LIST_SLUG_LEN] = {0};
    if (slug && slug[0]) {
        strlcpy(target, slug, sizeof(target));
    } else {
        if (pin_lists_get_active(target) != ESP_OK) {
            ESP_LOGW(TAG, "play_pinned_channel: no active list");
            return ESP_ERR_NOT_FOUND;
        }
    }

    ESP_LOGI(TAG, "play_pinned_channel: %s", target);

    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) return ESP_ERR_NO_MEM;
    playset->channel_count = 1;
    playset->channels[0].type = PS_CHANNEL_TYPE_PINNED;
    strlcpy(playset->channels[0].name, "pinned", sizeof(playset->channels[0].name));
    strlcpy(playset->channels[0].identifier, target, sizeof(playset->channels[0].identifier));
    playset->channels[0].weight = 1;
    ps_ensure_display_name(&playset->channels[0]);

    esp_err_t result = play_scheduler_execute_playset(playset);
    free(playset);
    return result;
}

esp_err_t play_scheduler_play_user_channel(const char *user_sqid)
{
    if (!user_sqid) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_user_channel: %s", user_sqid);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Failed to allocate playset struct");
        return ESP_ERR_NO_MEM;
    }

    playset->channel_count = 1;

    playset->channels[0].type = PS_CHANNEL_TYPE_USER;
    strlcpy(playset->channels[0].name, "user", sizeof(playset->channels[0].name));
    strlcpy(playset->channels[0].identifier, user_sqid, sizeof(playset->channels[0].identifier));
    playset->channels[0].weight = 1;
    ps_ensure_display_name(&playset->channels[0]);

    esp_err_t result = play_scheduler_execute_playset(playset);
    free(playset);
    return result;
}

esp_err_t play_scheduler_play_reactions_channel(const char *user_sqid)
{
    if (!user_sqid) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_reactions_channel: %s", user_sqid);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Failed to allocate playset struct");
        return ESP_ERR_NO_MEM;
    }

    playset->channel_count = 1;

    playset->channels[0].type = PS_CHANNEL_TYPE_REACTIONS;
    strlcpy(playset->channels[0].name, "reactions", sizeof(playset->channels[0].name));
    strlcpy(playset->channels[0].identifier, user_sqid, sizeof(playset->channels[0].identifier));
    playset->channels[0].weight = 1;
    ps_ensure_display_name(&playset->channels[0]);

    esp_err_t result = play_scheduler_execute_playset(playset);
    free(playset);
    return result;
}

esp_err_t play_scheduler_play_hashtag_channel(const char *hashtag)
{
    if (!hashtag) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "play_hashtag_channel: %s", hashtag);

    // Heap allocate to avoid ~4.6KB stack usage (called from 8KB stack tasks)
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Failed to allocate playset struct");
        return ESP_ERR_NO_MEM;
    }

    playset->channel_count = 1;

    playset->channels[0].type = PS_CHANNEL_TYPE_HASHTAG;
    strlcpy(playset->channels[0].name, "hashtag", sizeof(playset->channels[0].name));
    strlcpy(playset->channels[0].identifier, hashtag, sizeof(playset->channels[0].identifier));
    playset->channels[0].weight = 1;
    ps_ensure_display_name(&playset->channels[0]);

    esp_err_t result = play_scheduler_execute_playset(playset);
    free(playset);
    return result;
}

esp_err_t play_scheduler_refresh_sdcard_cache(void)
{
    ESP_LOGI(TAG, "Refreshing SD card cache");

    // Step 1: rebuild the on-disk sdcard.bin index by rescanning /animations.
    esp_err_t err = ps_build_sdcard_index();
    if (err != ESP_OK) {
        return err;
    }

    // Step 2: reload the freshly written index into every active SD-card
    // channel's in-memory state. Without this, ch->entries / ch->entry_count /
    // ch->available_count keep their pre-rebuild values and the picker, the
    // auto-swap availability gate, and the web UI all see a stale count
    // (e.g. "34/34" after the user copied files up to 124 over USB MSC).
    //
    // The scheduler mutex serialises us against play_scheduler_next() and the
    // background refresh task, both of which read ch->entries under the same
    // mutex — safe to free() and reallocate the entry buffer here.
    ps_state_t *s_state = ps_get_state();
    if (!s_state || !s_state->initialized) {
        return ESP_OK;
    }

    xSemaphoreTake(s_state->mutex, portMAX_DELAY);
    bool reloaded_any = false;
    for (size_t i = 0; i < s_state->channel_count; i++) {
        ps_channel_state_t *ch = &s_state->channels[i];
        if (ch->type != PS_CHANNEL_TYPE_SDCARD) {
            continue;
        }
        esp_err_t load_err = ps_load_channel_cache(ch);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Channel '%s': reload after rebuild failed: %s",
                     ch->display_name, esp_err_to_name(load_err));
        }
        reloaded_any = true;
    }
    if (reloaded_any) {
        ps_swrr_calculate_weights(s_state);
    }
    xSemaphoreGive(s_state->mutex);

    return ESP_OK;
}

// ============================================================================
// Artwork Channel Functions
// ============================================================================

/**
 * @brief Build vault filepath from storage_key and art_url
 *
 * Computes the sharded vault path: {vault}/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
 */
static void ps_build_artwork_filepath(const char *storage_key, const char *art_url,
                                       char *out_path, size_t max_len)
{
    if (!storage_key || !out_path || max_len == 0) {
        if (out_path && max_len > 0) out_path[0] = '\0';
        return;
    }

    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        strlcpy(vault_base, "/sdcard/p3a/vault", sizeof(vault_base));
    }

    // Compute SHA256 for sharding
    uint8_t sha256[32];
    if (mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), sha256, 0) != 0) {
        // Fallback without sharding
        snprintf(out_path, max_len, "%s/%s.webp", vault_base, storage_key);
        return;
    }

    // Detect extension from URL
    const char *ext = ".webp";
    if (art_url) {
        size_t url_len = strlen(art_url);
        if (url_len >= 4) {
            if (strcasecmp(art_url + url_len - 4, ".gif") == 0) ext = ".gif";
            else if (strcasecmp(art_url + url_len - 4, ".png") == 0) ext = ".png";
            else if (strcasecmp(art_url + url_len - 4, ".jpg") == 0) ext = ".jpg";
            else if (url_len >= 5 && strcasecmp(art_url + url_len - 5, ".jpeg") == 0) ext = ".jpg";
            else if (url_len >= 5 && strcasecmp(art_url + url_len - 5, ".webp") == 0) ext = ".webp";
        }
    }

    snprintf(out_path, max_len, "%s/%02x/%02x/%02x/%s%s",
             vault_base,
             (unsigned int)sha256[0],
             (unsigned int)sha256[1],
             (unsigned int)sha256[2],
             storage_key, ext);
}

esp_err_t play_scheduler_play_artwork(int32_t post_id,
                                      const char *storage_key,
                                      const char *art_url,
                                      const char *title)
{
    if (!storage_key || !art_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_state_t *s_state = ps_get_state();

    ESP_LOGI(TAG, "play_artwork: post_id=%ld, storage_key=%s, title='%s'",
             (long)post_id, storage_key, (title && title[0]) ? title : "");

    // Set view intent BEFORE execute_playset (for view tracking)
    extern void makapix_set_view_intent_intentional(bool intentional);
    makapix_set_view_intent_intentional(true);

    // Heap allocate to avoid ~4.6KB stack usage
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Failed to allocate playset struct");
        return ESP_ERR_NO_MEM;
    }

    playset->channel_count = 1;

    playset->channels[0].type = PS_CHANNEL_TYPE_ARTWORK;
    strlcpy(playset->channels[0].name, "artwork", sizeof(playset->channels[0].name));
    playset->channels[0].weight = 1;

    // Set artwork-specific fields
    playset->channels[0].artwork.post_id = post_id;
    playset->channels[0].artwork.post_source = (post_id != 0) ? POST_SOURCE_MAKAPIX : POST_SOURCE_NONE;
    strlcpy(playset->channels[0].artwork.storage_key, storage_key, sizeof(playset->channels[0].artwork.storage_key));
    strlcpy(playset->channels[0].artwork.art_url, art_url, sizeof(playset->channels[0].artwork.art_url));

    // Compute vault filepath from storage_key
    ps_build_artwork_filepath(storage_key, art_url,
                               playset->channels[0].artwork.filepath,
                               sizeof(playset->channels[0].artwork.filepath));

    // Mark the upcoming swap loud-fail: the user picked a specific artwork,
    // so a load failure should surface an on-screen error instead of being
    // silently retried with a different artwork.
    s_state->next_swap_fail_mode = SWAP_FAIL_LOUD;

    esp_err_t result = play_scheduler_execute_playset(playset);
    if (result == ESP_OK) {
        // Treat the single-artwork session as a first-class active playset so
        // the WebUI shows it correctly, the preview URL builder fires, and
        // boot restore can replay it.
        p3a_state_set_active_artwork(post_id, storage_key, art_url, title);
    }
    free(playset);
    return result;
}

esp_err_t play_scheduler_play_local_file(const char *filepath)
{
    if (!filepath) {
        return ESP_ERR_INVALID_ARG;
    }

    // If the file is missing (e.g. on boot restore after an SD swap), fail
    // out so callers (boot restore in animation_player.c) can fall back.
    struct stat st;
    if (stat(filepath, &st) != 0) {
        ESP_LOGW(TAG, "play_local_file: file not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    ps_state_t *s_state = ps_get_state();

    ESP_LOGI(TAG, "play_local_file: %s", filepath);

    // Heap allocate to avoid ~4.6KB stack usage
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Failed to allocate playset struct");
        return ESP_ERR_NO_MEM;
    }

    playset->channel_count = 1;

    playset->channels[0].type = PS_CHANNEL_TYPE_ARTWORK;
    strlcpy(playset->channels[0].name, "artwork", sizeof(playset->channels[0].name));
    playset->channels[0].weight = 1;

    // Local files: no view tracking (post_id = 0), storage_key and art_url empty
    playset->channels[0].artwork.post_id = 0;
    playset->channels[0].artwork.storage_key[0] = '\0';
    playset->channels[0].artwork.art_url[0] = '\0';
    strlcpy(playset->channels[0].artwork.filepath, filepath, sizeof(playset->channels[0].artwork.filepath));

    // Mark the upcoming swap loud-fail: the user picked a specific local file,
    // so a load failure should surface an on-screen error instead of being
    // silently retried with a different artwork.
    s_state->next_swap_fail_mode = SWAP_FAIL_LOUD;

    esp_err_t result = play_scheduler_execute_playset(playset);
    if (result == ESP_OK) {
        p3a_state_set_active_local_file(filepath);
    }
    free(playset);
    return result;
}
