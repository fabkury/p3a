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

#ifdef CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW
#define PS_RANDOM_WINDOW CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW
#else
#define PS_RANDOM_WINDOW 64
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
 * @brief Channel types
 */
typedef enum {
    PS_CHANNEL_TYPE_NAMED,    // "all", "promoted"
    PS_CHANNEL_TYPE_USER,     // "user:{sqid}"
    PS_CHANNEL_TYPE_HASHTAG,  // "hashtag:{tag}"
    PS_CHANNEL_TYPE_SDCARD,   // "sdcard"
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
    int32_t post_id;              // Sequential negative ID (-1, -2, ...)
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
    char filepath[256];           // Local path to file
    char storage_key[96];         // Vault storage key
    uint32_t created_at;          // Unix timestamp
    uint32_t dwell_time_ms;       // Per-artwork dwell (0 = use default)
    asset_type_t type;            // WEBP, GIF, PNG, JPEG
    uint8_t channel_index;        // Which channel this came from
} ps_artwork_t;

/**
 * @brief Channel specification for scheduler commands
 *
 * Specifies a channel to include in a scheduler command.
 */
typedef struct {
    ps_channel_type_t type;       // NAMED, USER, HASHTAG, SDCARD
    char name[33];                // "all", "promoted", "user", "hashtag", "sdcard"
    char identifier[33];          // For USER: sqid, for HASHTAG: tag
    uint32_t weight;              // For MaE mode (0 = auto-calculate)
} ps_channel_spec_t;

/**
 * @brief Scheduler command
 *
 * Contains all parameters needed to produce a play queue.
 * Executing a command resets channel state but preserves history.
 */
typedef struct {
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
    char channel_id[64];      // Derived: "all", "user:uvz", etc.
    ps_channel_type_t type;   // Channel type
    void *handle;             // channel_handle_t (legacy)

    // SWRR state
    int32_t credit;
    uint32_t weight;          // Normalized weight (out of 65536)

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
    uint32_t total_count;       // From server (for PrE)
    uint32_t recent_count;      // From server (for PrE), 0 for SD card

} ps_channel_state_t;

#ifdef __cplusplus
}
#endif

#endif // PLAY_SCHEDULER_TYPES_H
