// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
#include "makapix_channel_impl.h"
#include "animation_swap_request.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Pick State Snapshot (for ps_peek_next_available)
// ============================================================================

/**
 * @brief Lightweight snapshot of mutable pick state
 *
 * Used by ps_peek_next_available() to save/restore only the fields that
 * ps_pick_next_available() might modify, instead of copying the entire
 * ps_state_t (~21KB) onto the stack.
 *
 * Size: ~1KB instead of ~21KB
 */
typedef struct {
    struct {
        int32_t credit;           // SWRR credit
        uint32_t cursor;          // RecencyPick cursor
        uint64_t pick_rng_state;  // RandomPick RNG state
    } channels[PS_MAX_CHANNELS];
    uint32_t epoch_id;
    int32_t last_played_id;
} ps_pick_snapshot_t;

// ============================================================================
// Internal State Structure
// ============================================================================

/**
 * @brief Full scheduler state (internal)
 */
typedef struct {
    // Configuration
    ps_pick_mode_t pick_mode;
    ps_channel_select_mode_t channel_select_mode;


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

    // NAE
    ps_nae_entry_t nae_pool[PS_NAE_POOL_SIZE];
    size_t nae_count;
    bool nae_enabled;

    // PRNG state (64-bit for proper PCG32)
    uint64_t prng_nae_state;
    uint64_t prng_pick_state;
    uint32_t global_seed;
    uint32_t epoch_id;

    // Repeat avoidance
    int32_t last_played_id;

    // Dwell time
    uint32_t dwell_time_seconds;

    // Timer
    TimerHandle_t dwell_timer;

    // Command gating
    SemaphoreHandle_t mutex;
    bool command_active;
    // Tracks whether THE initial swap for the current playset has been
    // emitted yet. The download manager used to maintain a parallel
    // `s_playback_initiated` for this same purpose, racing with the LAi
    // 0→1 trigger here; that flag was removed (S2). first_swap_emitted is
    // now the single source of truth, set only inside the mutex-protected
    // LAi first-available path, the refresh-complete paths, and execute-
    // playset (on a successful immediate pick). It is cleared at playset
    // teardown, and rolled back by play_scheduler_next()'s failure path
    // when an optimistically-set first swap failed before anything played
    // (empty history) — so the first-swap gates re-arm instead of
    // stranding a cold playset on a status screen.
    //
    // Invariant (S5): at the moment first_swap_emitted is set true, at
    // least one channel must have a downloaded artwork (Σ available_count
    // > 0). Eviction may legitimately drop the total back to zero later;
    // the invariant only constrains the set-time, not lifetime steady
    // state. ps_assert_first_swap_invariant() in play_scheduler_lai.c
    // logs ESP_LOGE if a code path ever violates it.
    bool first_swap_emitted;
    bool initialized;

    // One-shot fail-mode override consumed by prepare_and_request_swap(): set
    // to SWAP_FAIL_LOUD by play_scheduler_play_artwork() / play_local_file()
    // so the resulting swap_request fails loudly on load error. Cleared after
    // every prepare_and_request_swap() call so subsequent auto-swaps default
    // back to SWAP_FAIL_SILENT.
    swap_fail_mode_t next_swap_fail_mode;

    // Last-executed playset. Allocated in PSRAM on first execute_playset and
    // reused for subsequent executions; struct-copied from the caller-provided
    // playset under s_state->mutex. NULL until the first execute. Read via
    // play_scheduler_get_active_playset() which returns a snapshot copy.
    ps_playset_t *active_playset;
} ps_state_t;

// ============================================================================
// Global State Access
// ============================================================================

/**
 * @brief Get pointer to global scheduler state
 */
ps_state_t *ps_get_state(void);

/**
 * @brief Verify the first_swap_emitted invariant after a successful trigger
 *
 * Called at each of the four set-sites for `state->first_swap_emitted`
 * (LAi first-available add, execute-playset, async-refresh-complete,
 * sync-refresh-complete). Logs ESP_LOGE if total_available is zero when
 * the flag transitions to true. Defense-in-depth (S5): the gating checks at
 * each call site already enforce the invariant, this is a tripwire for
 * future refactors.
 *
 * Caller MUST hold state->mutex.
 *
 * @param state  Scheduler state (must be non-NULL).
 * @param origin Short string identifying the call site (e.g. "lai_zto",
 *               "execute_playset"). Included in the error log.
 */
void ps_assert_first_swap_invariant(ps_state_t *state, const char *origin);

/**
 * @brief Number of currently-playable artworks in a channel.
 *
 * Abstracts over how each channel type tracks availability so callers can
 * sum "what can we play right now" uniformly:
 *   - Makapix/Giphy/institution: cache->available_count (the LAi).
 *   - SD card: available_count (mirrors entry_count once scanned).
 *   - Artwork ("show this one"): has no cache and never populates
 *     available_count; its single item is playable iff ch->active (the same
 *     gate ps_pick_artwork uses), so this returns 1 when active, else 0.
 *
 * Prefer this over the raw `c->cache ? c->cache->available_count :
 * c->available_count` idiom so artwork channels aren't miscounted as empty.
 */
size_t ps_channel_available_count(const ps_channel_state_t *c);

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

/**
 * @brief Pick next available artwork from all channels
 *
 * Uses SWRR to select channel, then iterates entries to find first
 * with file_exists(). Returns false immediately if nothing available.
 *
 * @param state Scheduler state
 * @param out_artwork Output artwork reference
 * @return true if artwork found and returned
 */
bool ps_pick_next_available(ps_state_t *state, ps_artwork_t *out_artwork);

/**
 * @brief Peek next available artwork without advancing state
 *
 * Temporarily calls ps_pick_next_available and then restores
 * the mutable pick state (credits, cursors, RNG) so the caller's
 * state is unchanged. Uses a lightweight snapshot (~1KB) instead
 * of copying the entire ps_state_t (~21KB).
 *
 * @param state Scheduler state (modified then restored)
 * @param out_artwork Output artwork reference
 * @return true if artwork found
 */
bool ps_peek_next_available(ps_state_t *state, ps_artwork_t *out_artwork);

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
 * @brief Select next channel using stochastic weighted sampling
 *
 * Uses credit-biased probabilities for randomized channel selection
 * while preserving long-term exposure ratios via the credit feedback loop.
 *
 * @param state Scheduler state
 * @return Channel index, or -1 if no active channels
 */
int ps_stochastic_select_channel(ps_state_t *state);

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
esp_err_t ps_touch_cache_file(const char *channel_id, ps_channel_type_t type);

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
 * @brief Reset periodic refresh timer
 *
 * Called when a new playset is executed to trigger
 * immediate refresh and reset the 1-hour periodic timer.
 */
void ps_refresh_reset_timer(void);

/**
 * @brief Build cache file path (internal helper)
 */
void ps_build_cache_path_internal(const char *channel_id, char *out_path, size_t max_len);

/**
 * @brief Load cache file for a channel
 *
 * Loads .bin file if it exists and populates ch->entries.
 * Sets entry_count, cache_loaded, and active flags.
 *
 * @param ch Channel state to load cache into
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ps_load_channel_cache(ps_channel_state_t *ch);

// ============================================================================
// Utilities
// ============================================================================

/**
 * @brief PCG32 PRNG next value (requires 64-bit state)
 */
uint32_t ps_prng_next(uint64_t *state);

/**
 * @brief Seed PRNG (64-bit state, seed can be any value)
 */
void ps_prng_seed(uint64_t *state, uint64_t seed);

/**
 * @brief Build vault filepath for an entry
 *
 * Uses hash sharding: {vault}/{d0}/{d1}/{storage_key}.{ext}
 * (see sd_path_build_sharded()). Implemented in play_scheduler_pick.c
 */
void ps_build_vault_filepath(const makapix_channel_entry_t *entry,
                              char *out, size_t out_len);

/**
 * @brief Check if a file exists
 */
bool ps_file_exists(const char *path);

/**
 * @brief Build display name from channel spec fields
 */
void ps_get_display_name_from_spec(ps_channel_type_t type, const char *spec_name,
                                   const char *identifier, char *out_name, size_t max_len);

/**
 * @brief Get user-friendly display name from channel_id (lookup in scheduler state)
 */
void ps_get_display_name(const char *channel_id, char *out_name, size_t max_len);

/**
 * @brief Ensure a channel spec has a display_name populated
 *
 * If display_name is empty, generates one from type/name/identifier.
 */
void ps_ensure_display_name(ps_channel_spec_t *spec);

/**
 * @brief Build cache file path for a channel
 *
 * Builds path like: {channel_dir}/{safe_channel_id}.bin
 */
void ps_build_cache_path(const char *channel_id, char *out_path, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_INTERNAL_H
