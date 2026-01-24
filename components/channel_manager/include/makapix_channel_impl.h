// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef MAKAPIX_CHANNEL_IMPL_H
#define MAKAPIX_CHANNEL_IMPL_H

#include "channel_interface.h"
#include "download_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Makapix Channel - implements channel_interface
 *
 * This channel connects to the Makapix Club server via MQTT to discover
 * and cache artworks. Artworks are stored locally in a vault with SHA256-
 * based naming for deduplication.
 *
 * Key features:
 * - Paginated queries to remote server
 * - Local caching of artwork files and metadata
 * - Unified cache file (<channel_id>.cache) with Ci + LAi for fast loading
 * - Background refresh via MQTT
 * - Power-loss safe file operations
 */

/**
 * @brief Post kind stored in Makapix channel index
 */
typedef enum {
    MAKAPIX_INDEX_POST_KIND_ARTWORK = 0,
    MAKAPIX_INDEX_POST_KIND_PLAYLIST = 1,
} makapix_index_post_kind_t;

/**
 * @brief Channel post entry stored in the channel cache (fixed size, packed)
 *
 * Entries are persisted as part of the unified .cache file (Ci array).
 * Breaking change: this replaces the old artwork-only entry format.
 * No migration/versioning is provided by design (SD card is expected to be wiped).
 */
typedef struct __attribute__((packed)) {
    int32_t post_id;                 // Makapix post_id
    uint8_t kind;                    // makapix_index_post_kind_t
    uint8_t extension;               // For artwork posts only (0=webp, 1=gif, 2=png, 3=jpg)
    uint16_t filter_flags;           // Filter flags (NSFW, etc.) - 0 if unknown/not applicable

    uint32_t created_at;             // Unix timestamp (0 if unknown)
    uint32_t artwork_modified_at;    // Unix timestamp (0 if unknown, artwork posts only)

    int32_t total_artworks;          // Playlist posts: total artworks (0 if unknown)

    uint8_t storage_key_uuid[16];    // Artwork posts: UUID bytes (0 if unknown)

    uint8_t reserved[28];            // Reserved for future use (keeps struct 64 bytes)
} makapix_channel_entry_t;

_Static_assert(sizeof(makapix_channel_entry_t) == 64, "Makapix channel entry must be 64 bytes");

/**
 * @brief Create a new Makapix channel
 * 
 * @param channel_id UUID of the channel (e.g., "abc123-def456-...")
 * @param name Display name for the channel
 * @param vault_path Base path for vault storage (e.g., "/sdcard/p3a/vault" or custom)
 * @param channels_path Base path for channel data (e.g., "/sdcard/p3a/channel" or custom)
 * @return Channel handle or NULL on failure
 */
channel_handle_t makapix_channel_create(const char *channel_id, 
                                         const char *name,
                                         const char *vault_path,
                                         const char *channels_path);

/**
 * @brief Get channel ID
 * 
 * @param channel Channel handle
 * @return Channel ID string or NULL
 */
const char *makapix_channel_get_id(channel_handle_t channel);

/**
 * @brief Check if a background refresh is in progress
 *
 * @param channel Channel handle
 * @return true if refresh is ongoing
 */
bool makapix_channel_is_refreshing(channel_handle_t channel);

/**
 * @brief Stop a channel's background refresh task gracefully
 *
 * Signals the refresh task to stop and waits up to 5 seconds for graceful exit.
 * The channel handle remains valid after this call (not destroyed).
 *
 * @param channel Channel handle
 * @return ESP_OK if stopped successfully, ESP_ERR_TIMEOUT if task didn't exit gracefully
 */
esp_err_t makapix_channel_stop_refresh(channel_handle_t channel);

/**
 * @brief Count cached artworks for a channel
 *
 * This function first checks the in-memory cache registry (fast path).
 * If the channel isn't loaded, it reads the .cache file header to get counts.
 *
 * @param channel_id Channel ID (e.g., "all", "promoted")
 * @param channels_path Path to channels directory (e.g., "/sdcard/p3a/channel" or custom)
 * @param vault_path Path to vault directory (unused, kept for API compatibility)
 * @param out_total If not NULL, receives total index entries (Ci count)
 * @param out_cached If not NULL, receives count of locally cached artworks (LAi count)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no cache exists
 */
esp_err_t makapix_channel_count_cached(const char *channel_id,
                                        const char *channels_path,
                                        const char *vault_path,
                                        size_t *out_total,
                                        size_t *out_cached);

#ifdef __cplusplus
}
#endif

#endif // MAKAPIX_CHANNEL_IMPL_H

