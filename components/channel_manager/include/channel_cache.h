// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef CHANNEL_CACHE_H
#define CHANNEL_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "makapix_channel_impl.h"
#include "makapix_api.h"  // For makapix_post_t
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

// Include PSRAM allocation utilities for uthash custom allocator
#include "psram_alloc.h"

// Configure uthash to use SPIRAM-preferring allocator
// This must be defined BEFORE including uthash.h
#define uthash_malloc(sz) psram_malloc(sz)
#define uthash_free(ptr, sz) free(ptr)

#include "uthash.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Hash Node Structures (for O(1) lookups)
// ============================================================================

/**
 * @brief Hash node: post_id -> ci_index
 */
typedef struct {
    int32_t post_id;
    uint32_t ci_index;
    UT_hash_handle hh;
} ci_post_id_node_t;

/**
 * @brief Hash node: LAi membership (post_id set)
 */
typedef struct {
    int32_t post_id;
    UT_hash_handle hh;
} lai_post_id_node_t;

/**
 * @file channel_cache.h
 * @brief LAi (Locally Available index) + Ci (Channel index) unified cache system
 *
 * This module provides a unified persistence format for channel metadata and
 * availability tracking. Key features:
 *
 * - **Ci (Channel Index)**: Array of all known artworks from the channel
 *   - Hash table for O(1) lookup by post_id (rebuilt on load)
 * - **LAi (Locally Available index)**: Stores post_ids of downloaded artworks
 *   - Hash set for O(1) membership checking (rebuilt on load)
 *   - Enables O(1) random picks without filesystem I/O
 * - **Atomic persistence**: Write to .tmp file, compute CRC32, rename
 * - **Legacy migration**: Detect old format (version < 20), rebuild LAi
 * - **15-second debounce**: Dirty caches saved after 15s of inactivity
 *
 * File format v20 (binary, little-endian):
 *   [header: 44 bytes]
 *   [Ci entries: ci_count * 64 bytes]
 *   [LAi post_ids: lai_count * 4 bytes (int32_t)]
 */

// Magic number: 'P3AC' (p3a Cache)
#define CHANNEL_CACHE_MAGIC     0x50334143
#define CHANNEL_CACHE_VERSION   21  // Bumped to 21: removed dwell_time_ms and metadata_modified_at from entries

// Default maximum entries per channel (configurable via config_store)
#define CHANNEL_CACHE_DEFAULT_MAX_ENTRIES 1024

// Legacy macro for backward compatibility - prefer channel_cache_get_max_entries()
// NOTE: This macro calls a function, but the function uses in-memory caching
// so repeated calls are fast (single integer return after first load).
#define CHANNEL_CACHE_MAX_ENTRIES channel_cache_get_max_entries()

// Debounce interval for dirty cache persistence (15 seconds)
#define CHANNEL_CACHE_SAVE_DEBOUNCE_MS 15000

/**
 * @brief Get the configured maximum entries per channel
 *
 * Returns the runtime-configured maximum number of entries per channel cache.
 * This value is read from NVS once and cached in memory, so subsequent calls
 * are fast (single integer return).
 *
 * @return Maximum entries (defaults to 1024)
 */
uint32_t channel_cache_get_max_entries(void);

/**
 * @brief Cache file header (44 bytes, packed)
 *
 * All multi-byte values are little-endian.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;          // CHANNEL_CACHE_MAGIC (0x50334143)
    uint16_t version;        // Format version (currently 1)
    uint16_t flags;          // Reserved flags (0)
    uint32_t ci_count;       // Number of entries in Ci
    uint32_t lai_count;      // Number of entries in LAi
    uint32_t ci_offset;      // Byte offset to Ci array (relative to file start)
    uint32_t lai_offset;     // Byte offset to LAi array (relative to file start)
    uint32_t checksum;       // CRC32 of entire file (with this field set to 0)
    char channel_id[16];     // First 16 chars of channel_id for validation
} channel_cache_header_t;

_Static_assert(sizeof(channel_cache_header_t) == 44, "Cache header must be 44 bytes");

/**
 * @brief In-memory channel cache state
 *
 * This structure holds both Ci (all known artworks) and LAi (locally available
 * subset) for a single channel. The Play Scheduler uses LAi directly for picks.
 */
typedef struct channel_cache_s {
    // Ci - Channel Index (all known artworks)
    makapix_channel_entry_t *entries;   // Array of entries (NULL if empty)
    size_t entry_count;                 // Number of entries in Ci

    // Ci hash table (rebuilt on load, not persisted)
    ci_post_id_node_t *post_id_hash;          // Hash: post_id -> ci_index

    // LAi - Locally Available index (stores post_ids, not ci_indices)
    int32_t *available_post_ids;        // Array of post_ids (NULL if empty)
    size_t available_count;             // Number of available artworks

    // LAi hash set (rebuilt on load, not persisted)
    lai_post_id_node_t *lai_hash;       // Hash set for O(1) membership check

    // Metadata
    char channel_id[64];                // Full channel ID
    uint32_t cache_version;             // Version from last load
    bool dirty;                         // Needs persistence

    // Synchronization
    SemaphoreHandle_t mutex;            // Protects all fields
} channel_cache_t;

// ============================================================================
// Cache Lifecycle
// ============================================================================

/**
 * @brief Initialize the channel cache subsystem
 *
 * Must be called once at startup. Creates the save debounce timer.
 *
 * @return ESP_OK on success
 */
esp_err_t channel_cache_init(void);

/**
 * @brief Deinitialize the channel cache subsystem
 *
 * Saves any dirty caches and frees resources.
 */
void channel_cache_deinit(void);

/**
 * @brief Load a channel cache from disk
 *
 * Loads the cache file for the given channel. If the file doesn't exist or
 * is corrupted, initializes an empty cache. If the file uses the legacy
 * format (no header), migrates it to the new format with LAi rebuilt.
 *
 * @param channel_id Channel identifier (e.g., "all", UUID)
 * @param channels_path Base path for channel files (e.g., "/sdcard/p3a/channel")
 * @param vault_path Base path for vault files (e.g., "/sdcard/p3a/vault")
 * @param cache Pre-allocated cache structure to fill
 * @return ESP_OK on success (even if file missing - returns empty cache)
 *         ESP_ERR_NO_MEM on allocation failure
 *         ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t channel_cache_load(const char *channel_id,
                             const char *channels_path,
                             const char *vault_path,
                             channel_cache_t *cache);

/**
 * @brief Save a channel cache to disk
 *
 * Writes the cache atomically: .tmp file, CRC32, then rename.
 * Clears the dirty flag on success.
 *
 * @param cache Cache to save
 * @param channels_path Base path for channel files
 * @return ESP_OK on success
 */
esp_err_t channel_cache_save(const channel_cache_t *cache, const char *channels_path);

/**
 * @brief Free cache resources
 *
 * Releases memory for entries, available_post_ids, and hash tables. Does NOT save.
 *
 * @param cache Cache to free
 */
void channel_cache_free(channel_cache_t *cache);

// ============================================================================
// LAi Operations
// ============================================================================

/**
 * @brief Add an entry to LAi
 *
 * Called when a download completes. Thread-safe (takes mutex).
 *
 * @param cache Cache to modify
 * @param post_id Post ID of the now-available artwork
 * @return true if added, false if already present
 */
bool lai_add_entry(channel_cache_t *cache, int32_t post_id);

/**
 * @brief Remove an entry from LAi
 *
 * Called when a file is deleted or fails to load. Uses swap-and-pop for O(1).
 * Thread-safe (takes mutex).
 *
 * @param cache Cache to modify
 * @param post_id Post ID of the artwork to remove
 * @return true if removed, false if not present
 */
bool lai_remove_entry(channel_cache_t *cache, int32_t post_id);

/**
 * @brief Check if an entry is in LAi
 *
 * Thread-safe (takes mutex). Uses hash table for O(1) lookup.
 *
 * @param cache Cache to check
 * @param post_id Post ID to check
 * @return true if present in LAi
 */
bool lai_contains(const channel_cache_t *cache, int32_t post_id);

/**
 * @brief Rebuild LAi by scanning vault for existing files
 *
 * Used during migration from legacy format or when LAi may be stale.
 * Expensive O(n) operation - scans each Ci entry for file existence.
 *
 * @param cache Cache to rebuild
 * @param vault_path Base path for vault files
 * @return Number of available files found
 */
size_t lai_rebuild(channel_cache_t *cache, const char *vault_path);

// ============================================================================
// Ci Operations
// ============================================================================

/**
 * @brief Find Ci index by post_id
 *
 * @param cache Cache to search
 * @param post_id Post ID to find
 * @return Ci index, or UINT32_MAX if not found
 */
uint32_t ci_find_by_post_id(const channel_cache_t *cache, int32_t post_id);

/**
 * @brief Get entry from Ci by index
 *
 * @param cache Cache to read
 * @param ci_index Index into Ci
 * @return Pointer to entry, or NULL if invalid index
 */
const makapix_channel_entry_t *ci_get_entry(const channel_cache_t *cache, uint32_t ci_index);

// ============================================================================
// Persistence Scheduling
// ============================================================================

/**
 * @brief Schedule a cache save with debouncing
 *
 * Marks the cache as dirty and resets the 15-second debounce timer.
 * When the timer fires, all dirty caches are saved.
 *
 * @param cache Cache to mark dirty
 */
void channel_cache_schedule_save(channel_cache_t *cache);

/**
 * @brief Force save all dirty caches immediately
 *
 * Bypasses the debounce timer. Call this on shutdown or before unmounting.
 *
 * @param channels_path Base path for channel files
 */
void channel_cache_flush_all(const char *channels_path);

// ============================================================================
// Global Cache Registry
// ============================================================================

/**
 * @brief Register a cache for global management
 *
 * Registered caches are included in flush_all and debounced saves.
 *
 * @param cache Cache to register
 * @return ESP_OK on success
 */
esp_err_t channel_cache_register(channel_cache_t *cache);

/**
 * @brief Unregister a cache from global management
 *
 * @param cache Cache to unregister
 */
void channel_cache_unregister(channel_cache_t *cache);

/**
 * @brief Get total LAi count across all registered caches
 *
 * @return Sum of available_count for all registered caches
 */
size_t channel_cache_get_total_available(void);

/**
 * @brief Find a registered cache by channel ID
 *
 * Thread-safe lookup in the registry.
 *
 * @param channel_id Channel identifier to find
 * @return Pointer to cache if found, NULL otherwise
 */
channel_cache_t *channel_cache_registry_find(const char *channel_id);

/**
 * @brief Get next Ci entry that needs downloading (artwork, not in LAi)
 *
 * Iterates from cursor position to find the next artwork entry that is not
 * yet in the Locally Available index. The cursor is advanced past the
 * returned entry (or to end if none found).
 *
 * Thread-safe (takes cache mutex).
 *
 * @param cache The cache instance
 * @param cursor In/out cursor position (start at 0, updated on each call)
 * @param out_entry Output entry if found (copied, caller-owned)
 * @return ESP_OK if entry found, ESP_ERR_NOT_FOUND if no more missing entries
 */
esp_err_t channel_cache_get_next_missing(channel_cache_t *cache,
                                         uint32_t *cursor,
                                         makapix_channel_entry_t *out_entry);

// ============================================================================
// Batch Operations (for Makapix refresh)
// ============================================================================

/**
 * @brief Merge new entries into cache (for batch refresh)
 *
 * Merges new entries with existing entries, deduplicating by post_id+kind.
 * Updates artwork file timestamps if changed on server (deletes local file
 * to trigger re-download).
 * Saves merged entries to the unified .cache file atomically.
 * Thread-safe.
 *
 * @param cache Cache to update
 * @param posts Array of posts from server
 * @param count Number of posts
 * @param channels_path Base path for channel files
 * @param vault_path Base path for vault files (for deleting outdated artwork files)
 * @return ESP_OK on success
 */
esp_err_t channel_cache_merge_posts(channel_cache_t *cache,
                                    const makapix_post_t *posts,
                                    size_t count,
                                    const char *channels_path,
                                    const char *vault_path);

/**
 * @brief Evict oldest downloaded artworks to stay within limit
 *
 * Finds downloaded artworks (by checking file existence), sorts by
 * created_at, and deletes oldest files until count <= max_count.
 * Updates LAi synchronously for each evicted file.
 *
 * @param cache Cache to update
 * @param max_count Maximum allowed downloaded artworks
 * @param vault_path Base path for vault files
 * @return Number of files evicted
 */
size_t channel_cache_evict_excess(channel_cache_t *cache,
                                  size_t max_count,
                                  const char *vault_path);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Compute CRC32 checksum
 *
 * Uses the standard CRC32 polynomial (0xEDB88320).
 *
 * @param data Data to checksum
 * @param len Length of data
 * @return CRC32 value
 */
uint32_t channel_cache_crc32(const void *data, size_t len);

/**
 * @brief Build cache file path
 *
 * @param channel_id Channel ID
 * @param channels_path Base path
 * @param out Output buffer
 * @param out_len Buffer length
 */
void channel_cache_build_path(const char *channel_id,
                              const char *channels_path,
                              char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_CACHE_H
