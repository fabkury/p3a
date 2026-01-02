// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_internal.h
 * @brief Internal definitions for Play Scheduler implementation
 *
 * This header is NOT part of the public API.
 */

#ifndef PLAY_SCHEDULER_INTERNAL_H
#define PLAY_SCHEDULER_INTERNAL_H

#include "play_scheduler.h"
#include "play_scheduler_types.h"
#include "channel_interface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Internal State Structure
// ============================================================================

/**
 * @brief Full scheduler state (internal)
 */
typedef struct {
    // Configuration
    ps_exposure_mode_t exposure_mode;
    ps_pick_mode_t pick_mode;

    // Channels
    ps_channel_state_t channels[PS_MAX_CHANNELS];
    size_t channel_count;

    // Current active channel (for single-channel mode)
    channel_handle_t current_channel;
    char current_channel_id[64];

    // Buffers
    ps_artwork_t *history;
    size_t history_head;
    size_t history_count;
    int32_t history_position;     // -1 = at head, 0+ = steps back

    ps_artwork_t *lookahead;
    size_t lookahead_head;
    size_t lookahead_tail;
    size_t lookahead_count;

    // NAE
    ps_nae_entry_t nae_pool[PS_NAE_POOL_SIZE];
    size_t nae_count;
    bool nae_enabled;

    // PRNG state
    uint32_t prng_nae_state;
    uint32_t prng_pick_state;
    uint32_t global_seed;
    uint32_t epoch_id;

    // Repeat avoidance
    int32_t last_played_id;

    // Dwell time
    uint32_t dwell_time_seconds;

    // Timer
    TaskHandle_t timer_task;
    volatile bool touch_next;
    volatile bool touch_back;

    // Command gating
    SemaphoreHandle_t mutex;
    bool command_active;
    bool initialized;
} ps_state_t;

// ============================================================================
// Global State Access
// ============================================================================

/**
 * @brief Get pointer to global scheduler state
 */
ps_state_t *ps_get_state(void);

// ============================================================================
// Buffer Operations (play_scheduler_buffers.c)
// ============================================================================

void ps_history_init(ps_state_t *state);
void ps_history_clear(ps_state_t *state);
void ps_history_push(ps_state_t *state, const ps_artwork_t *artwork);
bool ps_history_can_go_back(const ps_state_t *state);
bool ps_history_can_go_forward(const ps_state_t *state);
bool ps_history_go_back(ps_state_t *state, ps_artwork_t *out_artwork);
bool ps_history_go_forward(ps_state_t *state, ps_artwork_t *out_artwork);
bool ps_history_get_current(const ps_state_t *state, ps_artwork_t *out_artwork);
bool ps_history_is_at_head(const ps_state_t *state);

void ps_lookahead_init(ps_state_t *state);
void ps_lookahead_clear(ps_state_t *state);
bool ps_lookahead_is_empty(const ps_state_t *state);
bool ps_lookahead_is_low(const ps_state_t *state);
size_t ps_lookahead_count(const ps_state_t *state);
bool ps_lookahead_push(ps_state_t *state, const ps_artwork_t *artwork);
bool ps_lookahead_pop(ps_state_t *state, ps_artwork_t *out_artwork);
bool ps_lookahead_peek(const ps_state_t *state, size_t index, ps_artwork_t *out_artwork);
size_t ps_lookahead_peek_many(const ps_state_t *state, size_t max_count, ps_artwork_t *out_artworks);
bool ps_lookahead_rotate(ps_state_t *state);

// ============================================================================
// Pick Operations (play_scheduler_pick.c)
// ============================================================================

/**
 * @brief Pick next artwork from channel using current pick mode
 *
 * @param state Scheduler state
 * @param channel_index Index of channel to pick from
 * @param out_artwork Output artwork reference
 * @return true if artwork was picked successfully
 */
bool ps_pick_artwork(ps_state_t *state, size_t channel_index, ps_artwork_t *out_artwork);

/**
 * @brief Reset pick state for a channel (cursor, etc.)
 */
void ps_pick_reset_channel(ps_state_t *state, size_t channel_index);

// ============================================================================
// SWRR Operations (play_scheduler_swrr.c)
// ============================================================================

/**
 * @brief Calculate channel weights based on exposure mode
 */
void ps_swrr_calculate_weights(ps_state_t *state);

/**
 * @brief Select next channel using SWRR algorithm
 *
 * @param state Scheduler state
 * @return Channel index, or -1 if no active channels
 */
int ps_swrr_select_channel(ps_state_t *state);

/**
 * @brief Reset SWRR credits
 */
void ps_swrr_reset_credits(ps_state_t *state);

// ============================================================================
// NAE Operations (play_scheduler_nae.c)
// ============================================================================

/**
 * @brief Insert artwork into NAE pool
 */
void ps_nae_insert(ps_state_t *state, const ps_artwork_t *artwork);

/**
 * @brief Try to select from NAE pool
 *
 * @param state Scheduler state
 * @param out_artwork Output artwork if selected
 * @return true if NAE was selected
 */
bool ps_nae_try_select(ps_state_t *state, ps_artwork_t *out_artwork);

/**
 * @brief Clear NAE pool
 */
void ps_nae_clear(ps_state_t *state);

// ============================================================================
// Timer Operations (play_scheduler_timer.c)
// ============================================================================

/**
 * @brief Start the auto-swap timer task
 */
esp_err_t ps_timer_start(ps_state_t *state);

/**
 * @brief Stop the timer task
 */
void ps_timer_stop(ps_state_t *state);

/**
 * @brief Reset timer countdown
 */
void ps_timer_reset(ps_state_t *state);

// ============================================================================
// Generation (play_scheduler.c)
// ============================================================================

/**
 * @brief Generate a batch of artworks into lookahead
 */
void ps_generate_batch(ps_state_t *state);

// ============================================================================
// Cache Operations (play_scheduler_cache.c)
// ============================================================================

/**
 * @brief Build SD card index file
 *
 * Scans /sdcard/p3a/animations/ and writes /sdcard/p3a/channel/sdcard.bin
 *
 * @return ESP_OK on success
 */
esp_err_t ps_build_sdcard_index(void);

/**
 * @brief Touch cache file to update mtime
 *
 * Used for LRU tracking of cache files.
 *
 * @param channel_id Channel ID
 * @return ESP_OK on success
 */
esp_err_t ps_touch_cache_file(const char *channel_id);

// ============================================================================
// Refresh Operations (play_scheduler_refresh.c)
// ============================================================================

/**
 * @brief Start background refresh task
 *
 * @return ESP_OK on success
 */
esp_err_t ps_refresh_start(void);

/**
 * @brief Stop background refresh task
 */
void ps_refresh_stop(void);

/**
 * @brief Signal that work is available for refresh task
 */
void ps_refresh_signal_work(void);

/**
 * @brief Build cache file path (internal helper)
 */
void ps_build_cache_path_internal(const char *channel_id, char *out_path, size_t max_len);

// ============================================================================
// Utilities
// ============================================================================

/**
 * @brief Simple PCG32 PRNG next value
 */
uint32_t ps_prng_next(uint32_t *state);

/**
 * @brief Seed PRNG
 */
void ps_prng_seed(uint32_t *state, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_INTERNAL_H
