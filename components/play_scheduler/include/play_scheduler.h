// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
 * - Deterministic and reproducible via reversible PRNGs
 *
 * The primary input is a playset (ps_playset_t): a declarative
 * configuration of which channels to include (with per-channel weights) and
 * how to pick artwork within channels.
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
// Playsets
// ============================================================================

/**
 * @brief Execute a playset
 *
 * This is the primary API for changing what the scheduler plays. A playset is
 * a declarative configuration of channels (with per-channel weights) and pick
 * mode.
 *
 * Resets channel state, preserves history, begins new play queue.
 *
 * @param playset Playset parameters
 * @param user_initiated true for explicit user actions (HTTP/touch playset
 *        switch, play_artwork, etc.) — the upcoming pick is marked
 *        SWAP_FAIL_LOUD so a "no playable files" giveup surfaces on screen
 *        instead of leaving the previous artwork in place. Pass false for
 *        boot-restore and other automatic call paths where the screen is
 *        empty anyway and silent retry is the correct default.
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_execute_playset(const ps_playset_t *playset, bool user_initiated);

// ============================================================================
// Asynchronous Playset Switching
// ============================================================================
//
// execute_playset takes seconds for large playsets (per-channel SD cache
// loads under the scheduler mutex). The request_* API hands the execution to
// a dedicated worker task so callers (HTTP/MQTT handlers) return immediately;
// progress and errors are observed via play_scheduler_get_switch_status(),
// which the /playsets/active endpoint folds into its polled payload + ETag.
//
// Requests coalesce latest-wins: a request enqueued while another is queued
// (not yet started) replaces it. All async requests run with
// user_initiated=true — they originate from explicit user actions.

/**
 * @brief Snapshot of the async switch state
 *
 * switching covers the whole request lifetime: set at enqueue, cleared when
 * the worker finishes the attempt (unless a newer request is already queued).
 * switch_seq increments once per completed attempt — success or failure —
 * so a poller can detect completion across its polling interval.
 * last_error_code is "" on success, else one of: PLAYSET_NOT_FOUND,
 * MQTT_TIMEOUT, NOT_CONNECTED, EXECUTE_ERROR, OOM, OTA_IN_PROGRESS.
 */
typedef struct {
    bool     switching;
    uint32_t switch_seq;
    char     pending_name[PS_PLAYSET_NAME_MAX + 1];
    char     last_error_code[24];
} ps_switch_status_t;

/**
 * @brief Request an async switch to a fully-resolved playset
 *
 * On ESP_OK, ownership of @p playset (a heap buffer, ideally PSRAM via
 * psram_calloc) transfers to the switch module — the caller must not access
 * or free it afterwards. On error the caller retains ownership.
 *
 * @param playset Heap-allocated playset; freed by the worker after execution
 * @param name    Display name for status reporting (e.g. the user-facing
 *                playset key); falls back to playset->name when NULL/empty
 * @return ESP_OK if enqueued; ESP_ERR_INVALID_ARG on bad playset;
 *         ESP_ERR_INVALID_STATE if the worker is not running
 */
esp_err_t play_scheduler_request_switch_by_value(ps_playset_t *playset, const char *name);

/**
 * @brief Request an async switch to a named playset
 *
 * The worker resolves the name itself: SD library first, then a Makapix
 * server fetch (which can take ~30 s of MQTT retries — the reason this
 * variant exists). Resolution failures surface via get_switch_status().
 *
 * @return ESP_OK if enqueued; ESP_ERR_INVALID_ARG on empty name;
 *         ESP_ERR_INVALID_STATE if the worker is not running
 */
esp_err_t play_scheduler_request_switch_by_name(const char *name);

/**
 * @brief Copy the current async switch status
 *
 * Never blocks on the scheduler state mutex — safe to call while a switch
 * is executing (that's the point). Reports a zeroed "not switching" status
 * if the worker was never started.
 */
void play_scheduler_get_switch_status(ps_switch_status_t *out);

/**
 * @brief Create a built-in single-channel playset
 *
 * Generates a playset for built-in playset names:
 * - "channel_recent": Single channel with "all" (user-facing: "All Makapix" /
 *                     "All Artworks") -- see naming note below
 * - "channel_promoted": Single channel with "promoted"
 * - "channel_sdcard": Single channel with sdcard
 *
 * These playsets are created locally without requiring server fetch.
 *
 * Naming note ("channel_recent" vs "all"):
 *   The built-in playset key is "channel_recent" but the underlying Makapix
 *   channel name is "all" and the user-facing label is "All Makapix" /
 *   "All Artworks". The "recent" wording is a legacy from when the channel
 *   was framed as time-sorted; it is in fact a list of all artworks. The
 *   playset key is preserved verbatim because it is persisted in NVS on
 *   existing devices (key "playset" under namespace "p3a_state") and is part
 *   of the wire format for POST /playset/{name}; renaming it would require an
 *   NVS migration / alias. The mapping channel_recent <-> "all" is hard-coded
 *   in:
 *     - components/play_scheduler/play_scheduler_playsets.c (ps_create_channel_playset)
 *     - components/http_api/http_api.c (legacy POST /channel)
 *     - components/http_api/http_api_rest_actions.c (GET /channel)
 *     - main/animation_player.c (boot-restore display name)
 *     - webui/index.html (pill bar id + label mapping)
 *
 * @param playset_name Name of the built-in playset
 * @param out_playset Output playset (caller must allocate)
 * @return ESP_OK if playset was created, ESP_ERR_NOT_FOUND if not a built-in playset
 */
esp_err_t ps_create_channel_playset(const char *playset_name, ps_playset_t *out_playset);

/**
 * @brief Convenience: Play a single named channel
 *
 * Creates a playset with one channel using RecencyPick.
 *
 * @param name "all", "promoted", or "sdcard"
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_named_channel(const char *name);

/**
 * @brief Convenience: Play a pinned-artwork list as a first-class channel
 *
 * Builds a one-channel playset with type PS_CHANNEL_TYPE_PINNED and the
 * given slug. The scheduler honours the global pick_mode (recency or random).
 *
 * @param slug 8-hex-char list slug, or NULL/"" to play the active list
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_pinned_channel(const char *slug);

/**
 * @brief Convenience: Play a user channel
 *
 * @param user_sqid User's sqid
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_user_channel(const char *user_sqid);

/**
 * @brief Convenience: Play a reactions channel (posts a user has reacted to)
 *
 * @param user_sqid Target user's sqid
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_reactions_channel(const char *user_sqid);

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
 * Downloads if not cached. View tracking enabled for Makapix sources.
 *
 * @param post_id Post ID for view tracking (0 for no tracking)
 * @param storage_key UUID storage key
 * @param art_url Download URL for the artwork
 * @param title Optional post title for WebUI display (NULL or "" → no title).
 *              Caller must have already truncated to PS_ARTWORK_TITLE_MAX bytes.
 * @return ESP_OK on success
 */
esp_err_t play_scheduler_play_artwork(int32_t post_id,
                                      const char *storage_key,
                                      const char *art_url,
                                      const char *title);

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
 * @brief Set the active channel set
 *
 * Rebuilds the play queue. History is preserved across this call.
 * Resets: cursors, SWRR credits.
 *
 * @param channels Array of channel configurations
 * @param count Number of channels (1-64)
 * @return ESP_OK on success
 *
 * @deprecated Use play_scheduler_execute_playset() instead
 */
esp_err_t play_scheduler_set_channels(
    const ps_channel_config_t *channels,
    size_t count
);

/**
 * @brief Switch to a single channel (N=1 use case)
 *
 * Convenience wrapper for play_scheduler_set_channels() with count=1.
 *
 * @param channel_id Channel identifier ("all", "promoted", "sdcard", etc.)
 * @return ESP_OK on success
 *
 * @deprecated Use play_scheduler_play_named_channel() instead
 */
esp_err_t play_scheduler_play_channel(const char *channel_id);

/**
 * @brief Set channel selection mode
 *
 * Switches between SWRR (deterministic) and Stochastic (randomized) channel
 * selection for multi-channel playsets. Persists to NVS and takes effect
 * immediately on the next pick.
 *
 * @param mode PS_CHANNEL_SELECT_SWRR or PS_CHANNEL_SELECT_STOCHASTIC
 */
void play_scheduler_set_channel_select_mode(ps_channel_select_mode_t mode);

/**
 * @brief Apply channel selection mode without persisting
 *
 * Runtime-only update of the in-memory mode. Use when the caller has already
 * persisted the value through another path (e.g. a batched config save) and
 * only needs the scheduler to pick up the change.
 *
 * @param mode PS_CHANNEL_SELECT_SWRR or PS_CHANNEL_SELECT_STOCHASTIC
 */
void play_scheduler_apply_channel_select_mode(ps_channel_select_mode_t mode);

/**
 * @brief Get channel selection mode
 *
 * @return Current channel selection mode
 */
ps_channel_select_mode_t play_scheduler_get_channel_select_mode(void);

/**
 * @brief Set pick mode (per-channel artwork selection)
 *
 * Switches between Recency (newest-first cursor) and Random pick modes.
 * Persists to NVS via config_store and applies on the next swap. The setting
 * is global — it survives playset switches.
 *
 * @param mode PS_PICK_RECENCY or PS_PICK_RANDOM
 */
void play_scheduler_set_pick_mode(ps_pick_mode_t mode);

/**
 * @brief Apply pick mode without persisting
 *
 * Runtime-only update of the in-memory mode. Use when the caller has already
 * persisted the value through another path (e.g. a batched config save).
 *
 * @param mode PS_PICK_RECENCY or PS_PICK_RANDOM
 */
void play_scheduler_apply_pick_mode(ps_pick_mode_t mode);

/**
 * @brief Get pick mode
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
 * Returns the channel IDs for all channels in the current playset.
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
 * @brief Compensate the recency cursor after a caller-side lai_remove_entry
 *
 * lai_remove_entry shifts LAi entries at positions > removed_pos left by one.
 * The channel's recency cursor must decrement when cursor > removed_pos to
 * keep referencing the same logical entry; cursor == removed_pos stays put
 * (the slot now holds the next-up entry). Takes s_state->mutex internally —
 * not safe to call while already holding it.
 *
 * Use this from refresh-side callers (orphan eviction during giphy / museum /
 * makapix refresh, etc.) that don't run inside the scheduler critical
 * section. Scheduler-internal callers already holding the state mutex
 * (pick, navigation, animation loader eviction) should adjust cursor inline
 * with `if (cursor > removed_pos) cursor--;`.
 *
 * @param cache The cache whose LAi was just modified
 * @param removed_pos The position returned via out_position from lai_remove_entry
 */
void play_scheduler_compensate_cursor_after_lai_remove(channel_cache_t *cache,
                                                       int removed_pos);

/**
 * @brief Notify scheduler that an LAi verification sweep completed
 *
 * Called by lai_verify (from the download task) after sweeping a channel's
 * LAi against disk. Clears the channel's pick-miss staleness window and,
 * when entries were evicted, recalculates SWRR weights so a channel whose
 * availability collapsed (possibly to zero) stops being selected.
 *
 * Takes s_state->mutex internally — not safe to call while holding it.
 * Unknown channel_id (playset switched mid-sweep) is a no-op.
 *
 * @param channel_id Channel that was swept
 * @param checked    Entries whose files were confirmed present
 * @param evicted    Entries removed from LAi (files missing)
 */
void play_scheduler_on_lai_swept(const char *channel_id, size_t checked, size_t evicted);

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
// Display Helpers
// ============================================================================

/**
 * @brief Compute channel_id = first 16 hex chars of SHA256("{type}:{name}:{identifier}:{offset}")
 *
 * Produces an opaque, filesystem-safe identifier from channel spec fields.
 * The offset is part of the canonical input so that the same source viewed
 * at different starting slices in different playsets occupies independent
 * cache directories and scheduler state. Pass 0 for callers that need the
 * "canonical" channel_id of a source (e.g. status lookups for offset=0).
 *
 * @param type Channel type enum
 * @param name Channel name (e.g. "all", "trending")
 * @param identifier Channel identifier (e.g. user sqid, hashtag)
 * @param offset Per-playset starting offset (0 for canonical/unsupported types)
 * @param out_id Output buffer for channel_id (17 chars used, but existing 64-char buffers are fine)
 * @param max_len Size of output buffer
 */
void ps_compute_channel_id(ps_channel_type_t type, const char *name,
                           const char *identifier, uint32_t offset,
                           char *out_id, size_t max_len);

/**
 * @brief Build display name from channel spec fields
 *
 * Assembles a human-readable display name from type, name, and identifier.
 *
 * @param type Channel type enum
 * @param spec_name Channel sub-type name (e.g. "all", "trending", "search")
 * @param identifier Channel identifier (e.g. user sqid, hashtag, search query)
 * @param out_name Output buffer for display name
 * @param max_len Size of output buffer
 */
void ps_get_display_name_from_spec(ps_channel_type_t type, const char *spec_name,
                                   const char *identifier, char *out_name, size_t max_len);

/**
 * @brief Get user-friendly display name from channel_id (lookup version)
 *
 * Finds the channel in the active scheduler state by channel_id and returns
 * its display name. Falls back to the raw channel_id if not found.
 *
 * @param channel_id Internal channel identifier (hex hash)
 * @param out_name Output buffer for display name
 * @param max_len Size of output buffer
 */
void ps_get_display_name(const char *channel_id, char *out_name, size_t max_len);

/**
 * @brief Resolve the display name of the channel a given artwork came from
 *
 * Prefers the stored display name of the matching channel in the active
 * scheduler state, matched by the artwork's stamped provenance (channel type +
 * spec name + identifier). This recovers the rich label the playset editor
 * composed — notably institution channels, whose human-readable section label
 * (e.g. "AIC · Impressionism") cannot be rebuilt from the artwork's spec name
 * and machine term id alone. Falls back to ps_get_display_name_from_spec() when
 * the channel is not in the active playset (e.g. a history item left over from
 * a previously loaded playset).
 *
 * @param artwork Artwork carrying stamped channel provenance
 * @param out_name Output buffer for display name
 * @param max_len Size of output buffer
 */
void play_scheduler_get_channel_display_name(const ps_artwork_t *artwork,
                                              char *out_name, size_t max_len);

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
 * @brief Get a snapshot copy of the currently active playset.
 *
 * Returns the most recent playset passed to play_scheduler_execute_playset().
 * The full ps_channel_spec_t (including the artwork sub-struct with post_id,
 * storage_key, art_url, filepath, and title) is preserved, so callers can
 * reconstruct the user-visible state without consulting any separate API.
 *
 * @param out_playset Caller-allocated playset that receives the copy.
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_ARG if out_playset is NULL;
 *         ESP_ERR_INVALID_STATE if the scheduler is not initialized;
 *         ESP_ERR_NOT_FOUND if no playset has been executed yet this session.
 */
esp_err_t play_scheduler_get_active_playset(ps_playset_t *out_playset);

/**
 * @brief Whether the given playset has channels the device can't play unregistered
 *
 * Returns true when the playset contains at least one non-open Makapix channel
 * (anything but the open "promoted" named channel, plus user/hashtag/reactions)
 * AND the device currently has no usable Makapix registration — i.e. Makapix is
 * in MAKAPIX_STATE_IDLE (never registered) or MAKAPIX_STATE_REGISTRATION_INVALID
 * (credentials rejected). In those states the refresh dispatcher permanently
 * gives up on those channels, leaving them empty; the web UI uses this to show a
 * "registration needed" banner. Pure read of Makapix state + the passed playset;
 * does not take the scheduler mutex.
 *
 * @param playset Playset to inspect (NULL returns false).
 * @return true if a registration-needed banner should be shown for this playset.
 */
bool play_scheduler_playset_needs_registration(const ps_playset_t *playset);

/**
 * @brief Copy the active single-artwork title into a caller buffer.
 *
 * Returns the title attached to the active playset when it's a single
 * PS_CHANNEL_TYPE_ARTWORK channel — used by the pin dispatcher to attach a
 * human-readable title to swipe-up pins. Writes an empty string if the active
 * playset is not a single-artwork playset (or no playset has been executed).
 *
 * @param out_title Caller buffer; written with NUL-terminated title.
 * @param len Length of out_title (must be > 0).
 */
void play_scheduler_get_active_artwork_title(char *out_title, size_t len);

/**
 * @brief Get per-channel detail snapshots for all active channels
 *
 * Copies channel state into caller-provided array under mutex.
 * Includes entry/available counts, refresh state, and last_refresh
 * timestamp from channel metadata sidecar files.
 *
 * @param out_channels  Caller-allocated array to receive details
 * @param max_count     Capacity of out_channels array
 * @param out_count     Receives actual number of channels written
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t play_scheduler_get_channel_details(
    ps_channel_detail_t *out_channels, size_t max_count, size_t *out_count);

/**
 * @brief Reset the scheduler
 *
 * Clears cursors, SWRR credits. Preserves history.
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

/**
 * @brief Check if a channel needs artwork downloads
 *
 * Returns true for both Makapix and Giphy channels (they download from network).
 * Returns false for SD card channel (files are already local).
 *
 * @param channel_id Channel ID
 * @return true if this channel needs downloads from network
 */
bool play_scheduler_needs_download(const char *channel_id);

/**
 * @brief Check if a channel is a Giphy channel
 *
 * @param channel_id Channel ID
 * @return true if this is a Giphy channel
 */
bool play_scheduler_is_giphy_channel(const char *channel_id);

/**
 * @brief Check if a channel is an art-institution (museum) channel
 *
 * @param channel_id Channel ID
 * @return true if this is an institution channel
 */
bool play_scheduler_is_institution_channel(const char *channel_id);

/**
 * @brief Check if a channel is a Klipy channel
 *
 * @param channel_id Channel ID
 * @return true if this is a Klipy channel
 */
bool play_scheduler_is_klipy_channel(const char *channel_id);

/**
 * @brief Look up the spec_name for a channel by channel_id
 *
 * Used by the download manager and other components that need the
 * playset's original spec_name ("artic:departments", "trending", "all",
 * ...) to drive type-specific URL / path construction without taking the
 * scheduler mutex themselves.
 *
 * @param channel_id Channel ID
 * @param out_spec_name Output buffer (33 bytes is sufficient)
 * @param max_len Size of output buffer
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t play_scheduler_get_channel_spec_name(const char *channel_id,
                                               char *out_spec_name, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_H
