// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler.h
 * @brief Play Scheduler - Deterministic playback engine for multi-channel artwork playback
 *
 * The Play Scheduler is a streaming generator that selects artworks from multiple
 * followed channels for presentation. Key features:
 *
 * - On-demand computation (no pre-built lookahead buffer)
 * - Availability masking: only locally downloaded files are visible
 * - History buffer for back-navigation
 * - Multi-channel fairness via Smooth Weighted Round Robin (SWRR)
 * - New Artwork Events (NAE) for responsive handling of new content
 * - Deterministic and reproducible via reversible PRNGs
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
// Scheduler Commands
// ============================================================================

/**
 * @brief Execute a scheduler command
 *
 * This is the primary API for changing what the scheduler plays.
 * Flushes lookahead, preserves history, begins new play queue.
 *
 * @param command Scheduler command parameters
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_execute_command(const ps_scheduler_command_t *command);

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

// Download Manager is now decoupled and owns its own state.
// Use download_manager_set_channels() to configure active channels.
// No lookahead-based prefetch - downloads work independently.

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

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_H
