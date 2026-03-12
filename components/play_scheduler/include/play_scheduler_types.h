// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef PLAY_SCHEDULER_TYPES_H
#define PLAY_SCHEDULER_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
} post_source_t;

/**
 * @brief Exposure modes for channel weighting
 */
typedef enum {
    PS_EXPOSURE_EQUAL,        // EqE: Equal exposure across channels
    PS_EXPOSURE_MANUAL,       // MaE: Manual weights
    PS_EXPOSURE_PROPORTIONAL, // PrE: Proportional with recency bias
} ps_exposure_mode_t;

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
    PS_CHANNEL_TYPE_NAMED,    // Named channels: spec_name = "all", "promoted"
    PS_CHANNEL_TYPE_USER,     // User channels: identifier = sqid
    PS_CHANNEL_TYPE_HASHTAG,  // Hashtag channels: identifier = tag
    PS_CHANNEL_TYPE_SDCARD,   // Local SD card files
    PS_CHANNEL_TYPE_ARTWORK,  // Single artwork (in-memory only)
    PS_CHANNEL_TYPE_GIPHY,    // Giphy channels: spec_name = "trending", "search"; identifier = query
} ps_channel_type_t;

/**
 * @brief Entry format types for channel cache
 *
 * Different channels use different binary formats for their cache files.
 */
typedef enum {
    PS_ENTRY_FORMAT_NONE,     // No entries loaded
    PS_ENTRY_FORMAT_MAKAPIX,  // makapix_channel_entry_t (64 bytes)
    PS_ENTRY_FORMAT_SDCARD,   // sdcard_index_entry_t (160 bytes)
    PS_ENTRY_FORMAT_GIPHY,    // giphy_channel_entry_t (64 bytes)
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
 * Used in: /sdcard/p3a/channel/sdcard.bin
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
    uint32_t dwell_time_ms;       // Per-artwork dwell (0 = use default) - CURRENTLY IGNORED, see note on ps_scheduler_command_t
    asset_type_t type;            // WEBP, GIF, PNG, JPEG
    uint8_t channel_index;        // Which channel this came from
    ps_channel_type_t channel_type; // Channel type for downstream use (PPA upscale branching)
} ps_artwork_t;

/**
 * @brief Channel specification for playsets
 *
 * Specifies a channel to include in a playset (scheduler command).
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
    uint32_t weight;              // For MaE mode (0 = auto-calculate)

    // Artwork-specific fields (only when type == PS_CHANNEL_TYPE_ARTWORK)
    struct {
        int32_t post_id;          // Post ID for view tracking (0 = local file)
        post_source_t post_source; // Source of the post_id
        char storage_key[64];     // UUID storage key (empty for local files)
        char art_url[256];        // Download URL (empty if cached or local)
        char filepath[256];       // Full path (computed or provided directly)
    } artwork;
} ps_channel_spec_t;

/**
 * @brief Scheduler command (also known as "playset")
 *
 * A playset is a declarative configuration that tells the Play Scheduler what
 * to play. It contains all parameters needed to produce a play queue:
 * - Which channels to include (up to PS_MAX_CHANNELS)
 * - How to balance exposure across channels (exposure_mode)
 * - How to pick artwork within each channel (pick_mode)
 *
 * Executing a playset resets channel state (cursors, SWRR credits) but
 * preserves playback history for back-navigation.
 *
 * The terms "scheduler command" and "playset" are interchangeable throughout
 * the codebase.
 *
 * NOTE ON DWELL TIME: Playsets intentionally do NOT include dwell time settings.
 * Currently p3a only supports a single globally-configured dwell time (set via
 * config_store). Per-playset, per-channel, and per-artwork dwell times are
 * deferred until a future design decision is made about dwell time handling.
 * See config_store_get_dwell_time() for the current implementation.
 */
#define PS_PLAYSET_NAME_MAX 32

typedef struct {
    char name[PS_PLAYSET_NAME_MAX + 1]; // Playset name (empty for transient/anonymous commands)
    ps_channel_spec_t channels[PS_MAX_CHANNELS];
    size_t channel_count;
    ps_exposure_mode_t exposure_mode;
    ps_pick_mode_t pick_mode;
} ps_scheduler_command_t;

/**
 * @brief Channel configuration for set_channels() (legacy)
 */
typedef struct {
    char channel_id[64];          // "all", "promoted", "sdcard", etc.
    uint32_t weight;              // For MaE mode (0 = auto-calculate)
    uint32_t total_count;         // From server or local scan
    uint32_t recent_count;        // From server (0 for SD card)
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
    ps_exposure_mode_t exposure_mode;
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
    uint32_t spec_weight;     // Original weight from playset spec (for MaE recalculation)

    // Pick state
    uint32_t cursor;          // For RecencyPick
    uint64_t pick_rng_state;  // For RandomPick (64-bit state for proper PCG32)

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
    uint32_t total_count;       // From server (for PrE)
    uint32_t recent_count;      // From server (for PrE), 0 for SD card

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
