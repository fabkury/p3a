// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file play_scheduler_types.h
 * @brief Play scheduler type definitions: channel specs, artwork refs, stats, commands
 */

#ifndef PLAY_SCHEDULER_TYPES_H
#define PLAY_SCHEDULER_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "sdcard_channel.h"  // For asset_type_t

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration Defaults
// ============================================================================

#ifdef CONFIG_PLAY_SCHEDULER_HISTORY_SIZE
#define PS_HISTORY_SIZE CONFIG_PLAY_SCHEDULER_HISTORY_SIZE
#else
#define PS_HISTORY_SIZE 32
#endif

#ifdef CONFIG_PLAY_SCHEDULER_NAE_POOL_SIZE
#define PS_NAE_POOL_SIZE CONFIG_PLAY_SCHEDULER_NAE_POOL_SIZE
#else
#define PS_NAE_POOL_SIZE 32
#endif

#ifdef CONFIG_PLAY_SCHEDULER_MAX_CHANNELS
#define PS_MAX_CHANNELS CONFIG_PLAY_SCHEDULER_MAX_CHANNELS
#else
#define PS_MAX_CHANNELS 64
#endif

/**
 * @brief Maximum length (bytes excluding the NUL terminator) of the artwork
 *        title attached to a PS_CHANNEL_TYPE_ARTWORK channel. Callers passing
 *        a title must pre-truncate; downstream code does not re-truncate.
 */
#define PS_ARTWORK_TITLE_MAX 128

// ============================================================================
// Enumerations
// ============================================================================

/**
 * @brief Source of a post_id (replaces sign-based convention)
 */
typedef enum {
    POST_SOURCE_NONE = 0,     // No post_id (unset/unknown)
    POST_SOURCE_MAKAPIX,      // Makapix platform post
    POST_SOURCE_GIPHY,        // Giphy trending/search
    POST_SOURCE_SDCARD,       // Local SD card file
    POST_SOURCE_INSTITUTION,  // Art institution (museum) artwork
} post_source_t;

/**
 * @brief Pick modes for per-channel artwork selection
 */
typedef enum {
    PS_PICK_RECENCY,          // Newest -> older cursor
    PS_PICK_RANDOM,           // Random from window
} ps_pick_mode_t;

/**
 * @brief Channel selection modes for multi-channel playsets
 */
typedef enum {
    PS_CHANNEL_SELECT_SWRR,        // Smooth Weighted Round Robin (deterministic with random tie-breaking)
    PS_CHANNEL_SELECT_STOCHASTIC,  // Weighted random sampling with credit drift correction
} ps_channel_select_mode_t;

/**
 * @brief Channel types
 */
typedef enum {
    // NOTE: ordinals are persisted in playset files (playset_store.c stores as uint8_t).
    // Only append new values at the end. Do not reorder or insert mid-enum.
    PS_CHANNEL_TYPE_NAMED = 0,    // Named channels: spec_name = "all", "promoted"
    PS_CHANNEL_TYPE_USER = 1,     // User channels: identifier = sqid
    PS_CHANNEL_TYPE_HASHTAG = 2,  // Hashtag channels: identifier = tag
    PS_CHANNEL_TYPE_SDCARD = 3,   // Local SD card files
    PS_CHANNEL_TYPE_ARTWORK = 4,  // Single artwork (in-memory only)
    PS_CHANNEL_TYPE_GIPHY = 5,    // Giphy channels: spec_name = "trending", "search"; identifier = query
    PS_CHANNEL_TYPE_REACTIONS = 6,// Reactions channels: identifier = sqid of target user
    PS_CHANNEL_TYPE_INSTITUTION = 7, // Art-institution channels: spec_name = "{museum}:{axis}", identifier = term_id
    PS_CHANNEL_TYPE_PINNED = 8,   // Pinned-artworks list: identifier = list slug
} ps_channel_type_t;

/**
 * @brief Entry format types for channel cache
 *
 * Different channels use different binary formats for their cache files.
 */
typedef enum {
    PS_ENTRY_FORMAT_NONE,         // No entries loaded
    PS_ENTRY_FORMAT_MAKAPIX,      // makapix_channel_entry_t (64 bytes)
    PS_ENTRY_FORMAT_SDCARD,       // sdcard_index_entry_t (160 bytes)
    PS_ENTRY_FORMAT_GIPHY,        // giphy_channel_entry_t (64 bytes)
    PS_ENTRY_FORMAT_INSTITUTION,  // institution_channel_entry_t (64 bytes)
    PS_ENTRY_FORMAT_PINNED,       // pinned_order_entry_t (64 bytes)
} ps_entry_format_t;

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief SD card index entry (160 bytes)
 *
 * Optimized format for local SD card files. Unlike Makapix entries, this
 * stores the full filename directly since local files are identified by
 * their names, not UUIDs.
 *
 * Used in: {sd-root}/channel/sdcard.bin (default root /sdcard/p3a)
 */
typedef struct __attribute__((packed)) {
    int32_t post_id;              // Sequential positive ID (1, 2, ...)
    uint8_t extension;            // 0=webp, 1=gif, 2=png, 3=jpg
    uint8_t kind;                 // Always 0 (artwork) for now
    uint8_t reserved1[2];         // Padding for alignment
    uint32_t created_at;          // File mtime (Unix timestamp)
    uint32_t dwell_time_ms;       // 0 = use default
    char filename[144];           // Null-terminated filename (max 143 chars + null)
} sdcard_index_entry_t;

_Static_assert(sizeof(sdcard_index_entry_t) == 160, "SD card index entry must be 160 bytes");

/**
 * @brief Artwork reference for playback
 *
 * Contains all information needed to load and display an artwork.
 */
typedef struct {
    int32_t artwork_id;           // Globally unique artwork ID
    int32_t post_id;              // Post ID for view tracking
    post_source_t post_source;    // Source of the post_id
    char filepath[256];           // Local path to file
    char storage_key[96];         // Vault storage key
    uint32_t created_at;          // Unix timestamp
    uint32_t dwell_time_ms;       // Per-artwork dwell (0 = use default) - CURRENTLY IGNORED, see note on ps_playset_t
    asset_type_t type;            // WEBP, GIF, PNG, JPEG
    uint8_t channel_index;        // Which channel this came from
    ps_channel_type_t channel_type; // Channel type for downstream use (PPA upscale branching)
    // Channel provenance, stamped at pick time so the view tracker can report
    // which channel a post came from even after stochastic selection picks a
    // different channel from the playset on the next swap.
    char channel_spec_name[33];   // Channel sub-type name ("all", "promoted", "user", "hashtag", "reactions", "sdcard", "trending", ...)
    char channel_identifier[33];  // USER/REACTIONS: sqid; HASHTAG: tag; empty otherwise
} ps_artwork_t;

/**
 * @brief Channel specification for playsets
 *
 * Specifies a channel to include in a playset.
 */
typedef struct {
    ps_channel_type_t type;       // NAMED, USER, HASHTAG, SDCARD, ARTWORK, GIPHY
    char name[33];                // Type-specific sub-name:
                                  //   NAMED: "all", "promoted"
                                  //   GIPHY: "trending" or "search"
                                  //   Others: type label ("user", "hashtag", "sdcard", "artwork")
    char identifier[33];          // Type-specific parameter:
                                  //   USER: sqid
                                  //   HASHTAG: tag
                                  //   GIPHY (search): search query (verbatim, spaces allowed)
    char display_name[65];        // Optional: friendly display name (e.g., user handle, hashtag)
    uint32_t weight;              // Channel weight; if all channels are 0, the scheduler distributes equally
    uint32_t offset;              // Starting offset into the channel's source listing (per-playset slice).
                                  // Honored for INSTITUTION, GIPHY, SDCARD, PINNED. Modulo against the
                                  // source's total record count at refresh-time wraps oversized values
                                  // back to the start. Always 0 for unsupported channel types.

    // Artwork-specific fields (only when type == PS_CHANNEL_TYPE_ARTWORK)
    struct {
        int32_t post_id;          // Post ID for view tracking (0 = local file)
        post_source_t post_source; // Source of the post_id
        char storage_key[64];     // UUID storage key (empty for local files)
        char art_url[256];        // Download URL (empty if cached or local)
        char filepath[256];       // Full path (computed or provided directly)
        char title[PS_ARTWORK_TITLE_MAX + 1]; // Post title for WebUI display.
                                              // Empty for local files or untitled artworks.
    } artwork;
} ps_channel_spec_t;

/**
 * @brief Playset
 *
 * A playset is a declarative configuration that tells the Play Scheduler what
 * to play. It contains all parameters needed to produce a play queue:
 * - Which channels to include (up to PS_MAX_CHANNELS)
 * - Per-channel weights (channels[i].weight; all-zero falls back to equal)
 *
 * Executing a playset resets channel state (cursors, SWRR credits) but
 * preserves playback history for back-navigation.
 *
 * NOTE ON PLAYBACK-STYLE SETTINGS: Playsets intentionally do NOT include
 * pick_mode or dwell_time. Both are personal preferences about *how* to play
 * rather than *what* to play, so they live globally in config_store and apply
 * across playset switches. See config_store_get_pick_mode() and
 * config_store_get_dwell_time() for the current implementations.
 */
#define PS_PLAYSET_NAME_MAX 32

typedef struct {
    char name[PS_PLAYSET_NAME_MAX + 1]; // Playset name (empty for transient/anonymous playsets)
    ps_channel_spec_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
} ps_playset_t;

/**
 * @brief Channel configuration for set_channels() (legacy)
 */
typedef struct {
    char channel_id[64];          // "all", "promoted", "sdcard", etc.
    uint32_t weight;              // Channel weight; if all channels are 0, the scheduler distributes equally
} ps_channel_config_t;

/**
 * @brief Scheduler statistics
 */
typedef struct {
    size_t channel_count;
    size_t history_count;
    size_t nae_pool_count;
    uint32_t epoch_id;
    const char *current_channel_id;
    ps_pick_mode_t pick_mode;
    size_t total_available;             // Sum of |LAi| across channels
    size_t total_entries;               // Sum of |Ci| across channels
} ps_stats_t;

// ============================================================================
// Internal Types (used by implementation files)
// ============================================================================

// Forward declaration for channel_cache_t (defined in channel_cache.h)
struct channel_cache_s;
typedef struct channel_cache_s channel_cache_t;

/**
 * @brief NAE pool entry
 */
typedef struct {
    ps_artwork_t artwork;
    float priority;           // (0, 1]
    uint64_t insertion_time;  // For tie-breaking
} ps_nae_entry_t;

/**
 * @brief Per-channel state
 */
typedef struct {
    char channel_id[64];      // SHA256 hex hash (16 chars) of "{type}:{name}:{identifier}"
    char identifier[33];      // Original identifier from spec (USER: sqid, HASHTAG: tag, GIPHY search: query)
    char display_name[65];    // Human-readable display name
    char spec_name[33];       // Original name from ps_channel_spec_t (sub-type: "all", "trending", etc.)
    ps_channel_type_t type;   // Channel type
    void *handle;             // channel_handle_t (legacy)

    // SWRR state
    int32_t credit;
    uint32_t weight;          // Normalized weight (out of 65536)
    uint32_t spec_weight;     // Original weight from playset spec (for weight recalculation)
    uint32_t offset;          // Per-playset starting offset (mirrored from ps_channel_spec_t.offset).
                              // Refresh paths consult this to slice their fetch window. Already part of
                              // channel_id, so two playsets with different offsets get separate caches.

    // Pick state
    uint32_t cursor;          // For RecencyPick
    uint64_t pick_rng_state;  // For RandomPick (64-bit state for proper PCG32)

    // Pick-miss staleness tracking (RAM only, never persisted). Shift
    // register of recent swap-time outcomes (bit 1 = file missing); when
    // the recent miss rate trips the windowed threshold, an LAi
    // verification sweep is requested. See ps_track_pick_outcome().
    uint32_t miss_window;
    uint8_t miss_window_fill;  // Valid bits in miss_window (saturates at 32)

    // Cache info
    // For SD card channels: entries/entry_count/available_* are used directly
    // For Makapix channels: access ch->cache->* instead (cache may reallocate during merges)
    size_t entry_count;       // Mi: local cache size (SD card only)
    bool active;              // Has playable content?
    bool cache_loaded;        // .bin file loaded into memory?
    ps_entry_format_t entry_format;  // Format of loaded entries
    void *entries;            // Entry array (SD card only)

    // LAi (Locally Available index) - post_ids of downloaded artworks (SD card only)
    int32_t *available_post_ids;
    size_t available_count;

    // Channel cache for Makapix channels (NULL for SD card)
    // Access ch->cache->entries, ch->cache->entry_count, ch->cache->available_post_ids,
    // ch->cache->available_count directly to avoid stale pointers after batch merges.
    channel_cache_t *cache;

    // Refresh state
    bool refresh_pending;       // Queued for background refresh
    bool refresh_in_progress;   // Currently refreshing
    bool refresh_async_pending; // Waiting for Makapix async completion
    uint32_t refresh_start_tick;    // Tick when refresh_in_progress was set (for async timeout)
    uint8_t fail_streak;        // Consecutive refresh failures (0 = healthy); drives the
                                // exponential retry backoff in play_scheduler_refresh.c
    int64_t retry_at_us;        // esp_timer deadline gating the next attempt after a
                                // failure (0 = no backoff). Monotonic domain — immune
                                // to SNTP step adjustments.
    time_t last_refresh;        // Unix timestamp of last successful refresh (0 = never).
                                // Mirrors the channel_metadata sidecar; loaded at channel
                                // init and updated by the refresh task on actual refresh
                                // completion. Drives oldest-first picker fairness.

    // Artwork channel state (only when type == PS_CHANNEL_TYPE_ARTWORK)
    struct {
        int32_t post_id;
        char storage_key[64];
        char art_url[256];
        char filepath[256];
        bool download_pending;
        bool download_in_progress;
    } artwork_state;

} ps_channel_state_t;

/**
 * @brief Per-channel detail snapshot (for API responses)
 *
 * Lightweight copy of channel state for external consumption.
 * Populated by play_scheduler_get_channel_details() under mutex.
 */
typedef struct {
    char channel_id[64];
    char identifier[33];
    char display_name[65];
    char spec_name[33];       // Sub-type from spec (e.g. "all", "trending", "search")
    ps_channel_type_t type;
    size_t entry_count;       // Ci (total in cache)
    size_t available_count;   // LAi (locally available)
    bool refreshing;          // refresh_pending || refresh_in_progress || refresh_async_pending
    time_t last_refresh;      // From channel_metadata sidecar (0 = never)
} ps_channel_detail_t;

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_TYPES_H
