// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "makapix_channel_impl.h"
#include "makapix_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief File extensions enum
 */
typedef enum {
    EXT_WEBP = 0,
    EXT_GIF  = 1,
    EXT_PNG  = 2,
    EXT_JPEG = 3,
} file_extension_t;

/**
 * @brief Internal Makapix channel state
 *
 * NOTE: play_navigator was removed as part of Play Scheduler migration.
 * Navigation is now handled by Play Scheduler directly.
 * See play_scheduler.c for Live Mode deferred feature notes.
 */
typedef struct {
    struct channel_s base;           // Base channel (must be first)

    // Configuration
    char *channel_id;                // UUID of channel
    char *vault_path;                // Base vault path
    char *channels_path;             // Base channels path

    // Loaded entries
    makapix_channel_entry_t *entries;  // Array of entries from channel index file (<channel>.bin)
    size_t entry_count;              // Number of entries

    // Legacy playback fields (kept for binary compatibility, no longer used)
    uint32_t channel_dwell_override_ms;

    // Refresh state
    bool refreshing;                 // Background refresh in progress
    TaskHandle_t refresh_task;      // Background refresh task handle
    time_t last_refresh_time;        // Last successful refresh timestamp

    // Serialize channel index load/write to avoid races during unlink+rename window
    SemaphoreHandle_t index_io_lock;

} makapix_channel_t;

// Extension strings for building file paths
extern const char *s_ext_strings[];

// ============================================================================
// Utility functions (makapix_channel_utils.c)
// ============================================================================

/**
 * @brief Parse UUID string to 16 bytes (removes hyphens)
 */
bool uuid_to_bytes(const char *uuid_str, uint8_t *out_bytes);

/**
 * @brief Convert 16 bytes back to UUID string with hyphens
 */
void bytes_to_uuid(const uint8_t *bytes, char *out, size_t out_len);

/**
 * @brief Compute SHA256(storage_key) for Makapix vault sharding
 */
esp_err_t storage_key_sha256(const char *storage_key, uint8_t out_sha256[32]);

/**
 * @brief Best-effort ISO8601 UTC parser: "YYYY-MM-DDTHH:MM:SSZ" -> time_t
 */
time_t parse_iso8601_utc(const char *s);

/**
 * @brief Build channel index path
 */
void build_index_path(const makapix_channel_t *ch, char *out, size_t out_len);

/**
 * @brief Build vault filepath for an artwork entry
 */
void build_vault_path(const makapix_channel_t *ch, 
                      const makapix_channel_entry_t *entry,
                      char *out, size_t out_len);

/**
 * @brief Build vault path from storage_key string
 */
void build_vault_path_from_storage_key(const makapix_channel_t *ch, 
                                        const char *storage_key, 
                                        file_extension_t ext, 
                                        char *out, size_t out_len);

/**
 * @brief Recover/cleanup channel index (.bin) and its temp (.bin.tmp)
 */
void makapix_index_recover_and_cleanup(const char *index_path);

/**
 * @brief Detect file type from URL
 */
file_extension_t detect_file_type(const char *url);

// ============================================================================
// Refresh functions (makapix_channel_refresh.c)
// ============================================================================

/**
 * @brief Background refresh task implementation
 */
void refresh_task_impl(void *pvParameters);

/**
 * @brief Update channel index (.bin) with new posts
 */
esp_err_t update_index_bin(makapix_channel_t *ch, const makapix_post_t *posts, size_t count);

/**
 * @brief Evict excess artworks beyond limit
 */
esp_err_t evict_excess_artworks(makapix_channel_t *ch, size_t max_count);

/**
 * @brief Save channel metadata JSON
 */
esp_err_t save_channel_metadata(makapix_channel_t *ch, const char *cursor, time_t refresh_time);

/**
 * @brief Load channel metadata JSON
 */
esp_err_t load_channel_metadata(makapix_channel_t *ch, char *out_cursor, time_t *out_refresh_time);

// ============================================================================
// Helper functions (shared across modules)
// ============================================================================

/**
 * @brief Compute effective dwell time with override cascade
 */
uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                    uint32_t channel_override_ms,
                                    uint32_t playlist_or_artwork_ms);

#ifdef __cplusplus
}
#endif

