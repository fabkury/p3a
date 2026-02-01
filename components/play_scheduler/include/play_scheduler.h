// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler.h
 * @brief Play Scheduler - Deterministic playback engine for multi-channel artwork playback
 *
 * The Play Scheduler is a streaming generator that selects artworks from multiple
 * followed channels for presentation. Key features:
 *
 * - On-demand computation via next()/peek() API
 * - Availability masking: only locally downloaded files are visible
 * - History buffer for back-navigation
 * - Multi-channel fairness via Smooth Weighted Round Robin (SWRR)
 * - New Artwork Events (NAE) for responsive handling of new content
 * - Deterministic and reproducible via reversible PRNGs
 *
 * Terminology: A "playset" is the preferred term for a scheduler command
 * (ps_scheduler_command_t). It describes what to play: which channels, how to
 * balance exposure, and how to pick artwork within channels.
 *
 * @see docs/play-scheduler/SPECIFICATION.md for full details
 */

#ifndef PLAY_SCHEDULER_H
#define PLAY_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "play_scheduler_types.h"
#include "makapix_channel_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize the Play Scheduler
 *
 * Allocates buffers and initializes internal state.
 * Must be called before any other play_scheduler functions.
 *
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_init(void);

/**
 * @brief Deinitialize and free all resources
 */
void play_scheduler_deinit(void);

// ============================================================================
// Scheduler Commands (Playsets)
// ============================================================================

/**
 * @brief Execute a scheduler command (playset)
 *
 * This is the primary API for changing what the scheduler plays. A "playset"
 * is the preferred term for a scheduler command - it describes a declarative
 * configuration of channels, exposure mode, and pick mode.
 *
 * Resets channel state, preserves history, begins new play queue.
 *
 * @param command Playset (scheduler command) parameters
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_execute_command(const ps_scheduler_command_t *command);

/**
 * @brief Create a built-in single-channel playset
 *
 * Generates a scheduler command for built-in playset names:
 * - "channel_recent": Single channel with "all" (Recent Artworks)
 * - "channel_promoted": Single channel with "promoted"
 * - "channel_sdcard": Single channel with sdcard
 *
 * These playsets are created locally without requiring server fetch.
 *
 * @param playset_name Name of the built-in playset
 * @param out_cmd Output scheduler command (caller must allocate)
 * @return ESP_OK if playset was created, ESP_ERR_NOT_FOUND if not a built-in playset
 */
esp_err_t ps_create_channel_playset(const char *playset_name, ps_scheduler_command_t *out_cmd);

/**
 * @brief Convenience: Play a single named channel
 *
 * Creates a command with one channel in EqE mode with RecencyPick.
 *
 * @param name "all", "promoted", or "sdcard"
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_named_channel(const char *name);

/**
 * @brief Convenience: Play a user channel
 *
 * @param user_sqid User's sqid
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_user_channel(const char *user_sqid);

/**
 * @brief Convenience: Play a hashtag channel
 *
 * @param hashtag Hashtag (without #)
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_hashtag_channel(const char *hashtag);

/**
 * @brief Play a single Makapix artwork via the scheduler
 *
 * Creates a playset with one PS_CHANNEL_TYPE_ARTWORK channel.
 * Downloads if not cached. View tracking enabled if post_id > 0.
 *
 * @param post_id Post ID for view tracking (0 for no tracking)
 * @param storage_key UUID storage key
 * @param art_url Download URL for the artwork
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_artwork(int32_t post_id, const char *storage_key, const char *art_url);

/**
 * @brief Play a local file via the scheduler (no view tracking)
 *
 * @param filepath Full path to the local file
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_local_file(const char *filepath);

// ============================================================================
// Channel Configuration (Legacy)
// ============================================================================

/**
 * @brief Set the active channel set and exposure mode
 *
 * Rebuilds the play queue. History is preserved across this call.
 * Resets: cursors, SWRR credits, NAE pool.
 *
 * @param channels Array of channel configurations
 * @param count Number of channels (1-64)
 * @param mode Exposure mode (EqE, MaE, or PrE)
 * @return ESP_OK on success
 *
 * @deprecated Use play_scheduler_execute_command() instead
 */
esp_err_t play_scheduler_set_channels(
    const ps_channel_config_t *channels,
    size_t count,
    ps_exposure_mode_t mode
);

/**
 * @brief Switch to a single channel (N=1 use case)
 *
 * Convenience wrapper for play_scheduler_set_channels() with count=1.
 * Uses PS_EXPOSURE_EQUAL mode.
 *
 * @param channel_id Channel identifier ("all", "promoted", "sdcard", etc.)
 * @return ESP_OK on success
 *
 * @deprecated Use play_scheduler_play_named_channel() instead
 */
esp_err_t play_scheduler_play_channel(const char *channel_id);

/**
 * @brief Set pick mode for per-channel selection
 *
 * @param mode Pick mode (PS_PICK_RECENCY or PS_PICK_RANDOM)
 */
void play_scheduler_set_pick_mode(ps_pick_mode_t mode);

/**
 * @brief Get current pick mode
 *
 * @return Current pick mode
 */
ps_pick_mode_t play_scheduler_get_pick_mode(void);

// ============================================================================
// Cache Management
// ============================================================================

/**
 * @brief Trigger SD card channel refresh
 *
 * Called when files are uploaded or user switches to SD card.
 */
esp_err_t play_scheduler_refresh_sdcard_cache(void);

// ============================================================================
// Download Integration
// ============================================================================

// Download Manager is decoupled and owns its own state.
// Use download_manager_set_channels() to configure active channels.

/**
 * @brief Get list of active channel IDs
 *
 * Returns the channel IDs for all channels in the current scheduler command.
 * Used by LRU eviction to protect active channel files.
 *
 * @param out_ids Array of channel ID pointers (points to internal storage)
 * @param max_count Maximum number of IDs to return
 * @return Number of channel IDs returned
 */
size_t play_scheduler_get_active_channel_ids(const char **out_ids, size_t max_count);

// ============================================================================
// LAi (Locally Available index) Integration
// ============================================================================

/**
 * @brief Notify scheduler that a download completed
 *
 * Called by download_manager when a file is successfully downloaded.
 * Updates the LAi to include the newly available artwork.
 * May trigger playback if this is the first available artwork (zero-to-one transition).
 *
 * @param channel_id Channel the artwork belongs to
 * @param post_id Post ID of the downloaded artwork (for O(1) LAi lookup)
 */
void play_scheduler_on_download_complete(const char *channel_id, int32_t post_id);

/**
 * @brief Notify scheduler that a load failed
 *
 * Called by animation_player when a file fails to load.
 * Records failure in LTF, deletes the file, removes from LAi.
 * May trigger picking another artwork if LAi is non-empty.
 *
 * @param storage_key Storage key of the failed artwork (UUID string, needed for file deletion/LTF)
 * @param channel_id Channel the artwork belongs to
 * @param post_id Post ID of the failed artwork (for O(1) LAi removal, use 0 if unknown)
 * @param reason Failure reason (e.g., "decode_error", "file_missing")
 */
void play_scheduler_on_load_failed(const char *storage_key, const char *channel_id, int32_t post_id, const char *reason);

/**
 * @brief Get total available artwork count across all channels
 *
 * Returns the sum of LAi sizes for all active channels.
 *
 * @return Total number of available artworks
 */
size_t play_scheduler_get_total_available(void);

/**
 * @brief Get channel stats for a specific channel
 *
 * Returns entry_count (total) and available_count (cached/LAi) for a channel.
 * Uses O(1) lookup from in-memory LAi - no filesystem scanning.
 *
 * @param channel_id Channel ID ("all", "promoted", etc.)
 * @param out_total Output total entries (may be NULL)
 * @param out_cached Output available/cached entries (may be NULL)
 */
void play_scheduler_get_channel_stats(const char *channel_id, size_t *out_total, size_t *out_cached);

// ============================================================================
// Navigation
// ============================================================================

/**
 * @brief Get the next artwork for playback
 *
 * Advances playback position. Computes next available artwork on-demand
 * using availability masking (only locally downloaded files are visible).
 * Also calls animation_player_request_swap().
 *
 * @param out_artwork Artwork reference (can be NULL if only triggering swap)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no artworks available
 */
esp_err_t play_scheduler_next(ps_artwork_t *out_artwork);

/**
 * @brief Go back to previous artwork
 *
 * Only navigates within history buffer. Does not mutate generator state.
 *
 * @param out_artwork Artwork reference
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if at history start
 */
esp_err_t play_scheduler_prev(ps_artwork_t *out_artwork);

/**
 * @brief Peek at what play_scheduler_next() would return
 *
 * Simulates picking without modifying state. Useful for pre-caching.
 *
 * @param out_artwork Artwork reference
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no artworks available
 */
esp_err_t play_scheduler_peek_next(ps_artwork_t *out_artwork);

/**
 * @brief Get current artwork without navigation
 *
 * @param out_artwork Artwork reference
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_current(ps_artwork_t *out_artwork);

// ============================================================================
// NAE (New Artwork Events)
// ============================================================================

/**
 * @brief Enable/disable NAE
 *
 * @param enable true to enable
 */
void play_scheduler_set_nae_enabled(bool enable);

/**
 * @brief Check if NAE is enabled
 *
 * @return true if NAE is enabled
 */
bool play_scheduler_is_nae_enabled(void);

/**
 * @brief Insert a new artwork event (called from MQTT handler)
 *
 * Inserts artwork into NAE pool with 50% initial priority.
 * If artwork already exists, resets priority to 50%.
 *
 * @param artwork Artwork reference
 */
void play_scheduler_nae_insert(const ps_artwork_t *artwork);

// ============================================================================
// Timer & Dwell
// ============================================================================

/**
 * @brief Set dwell time for auto-swap
 *
 * @param seconds Dwell time (0 = disable auto-swap)
 */
void play_scheduler_set_dwell_time(uint32_t seconds);

/**
 * @brief Get current dwell time
 *
 * @return Dwell time in seconds
 */
uint32_t play_scheduler_get_dwell_time(void);

/**
 * @brief Reset the auto-swap timer (called after manual navigation)
 */
void play_scheduler_reset_timer(void);

/**
 * @brief Pause the auto-swap timer (for PICO-8 mode)
 *
 * Stops the dwell timer so no swap events fire during PICO-8 streaming.
 */
void play_scheduler_pause_auto_swap(void);

/**
 * @brief Resume the auto-swap timer (after PICO-8 mode)
 *
 * Restarts the dwell timer with the current dwell time.
 */
void play_scheduler_resume_auto_swap(void);

// ============================================================================
// Touch Events (lightweight signals from touch handler)
// ============================================================================

/**
 * @brief Signal touch-triggered next
 */
void play_scheduler_touch_next(void);

/**
 * @brief Signal touch-triggered back
 */
void play_scheduler_touch_back(void);

// ============================================================================
// Status & Debugging
// ============================================================================

/**
 * @brief Get scheduler statistics
 *
 * @param out_stats Pointer to receive statistics
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_get_stats(ps_stats_t *out_stats);

/**
 * @brief Reset the scheduler
 *
 * Clears cursors, SWRR credits, and NAE pool. Preserves history.
 * Increments epoch_id.
 */
void play_scheduler_reset(void);

/**
 * @brief Check if scheduler is initialized
 *
 * @return true if initialized
 */
bool play_scheduler_is_initialized(void);

// ============================================================================
// Download Manager Integration
// ============================================================================

/**
 * @brief Get entry count for a channel (for download scanning)
 *
 * Returns the total number of Ci entries for the specified channel.
 * Thread-safe - takes internal mutex.
 *
 * @param channel_id Channel ID ("all", "promoted", etc.)
 * @return Entry count, or 0 if channel not found
 */
size_t play_scheduler_get_channel_entry_count(const char *channel_id);

/**
 * @brief Get a channel entry by index (for download scanning)
 *
 * Copies the entry at the given index to the caller's buffer.
 * Thread-safe - takes internal mutex.
 *
 * @param channel_id Channel ID
 * @param index Entry index (0 to entry_count-1)
 * @param out_entry Output buffer for the entry
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if channel/index not found
 */
esp_err_t play_scheduler_get_channel_entry(const char *channel_id, size_t index,
                                            makapix_channel_entry_t *out_entry);

/**
 * @brief Check if a channel is a Makapix channel (not SD card)
 *
 * SD card channels don't need downloads. This allows download_manager
 * to skip SD card channels without hardcoding channel type checks.
 *
 * @param channel_id Channel ID
 * @return true if this is a Makapix channel that may need downloads
 */
bool play_scheduler_is_makapix_channel(const char *channel_id);

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_H
