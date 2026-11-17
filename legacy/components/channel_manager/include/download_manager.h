// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file download_manager.h
 * @brief Download manager interface: round-robin file fetching across channels
 */

#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "playlist_manager.h"
#include "makapix_channel_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Download request structure
 * 
 * Filled by the channel when providing the next file to download.
 */
typedef struct {
    char storage_key[96];
    char art_url[256];
    char filepath[256];           // Expected local path (vault)
    char channel_id[64];          // Channel this download belongs to
    int32_t post_id;              // Post ID for O(1) LAi lookup after download
} download_request_t;

/**
 * @brief Callback type for getting the next file to download
 * 
 * Called by the download manager to get the next file to download.
 * The channel should scan forward from current position and return
 * the first missing file.
 * 
 * @param out_request Filled with download info if a file needs downloading
 * @param user_ctx User context
 * @return ESP_OK if a file needs downloading (out_request filled)
 *         ESP_ERR_NOT_FOUND if all files are downloaded (nothing to do)
 *         Other error codes on failure
 */
typedef esp_err_t (*download_get_next_cb_t)(download_request_t *out_request, void *user_ctx);

/**
 * @brief Initialize download manager
 * 
 * Creates the download task which sleeps until work is available.
 * 
 * @return ESP_OK on success
 */
esp_err_t download_manager_init(void);

/**
 * @brief Deinitialize download manager
 */
void download_manager_deinit(void);

/**
 * @brief Set the callback for getting next download
 * 
 * The download manager calls this to get the next file to download.
 * 
 * @param cb Callback function
 * @param user_ctx User context passed to callback
 */
void download_manager_set_next_callback(download_get_next_cb_t cb, void *user_ctx);

/**
 * @brief Wake the download task to check for pending downloads
 *
 * Lightweight signal that just wakes the download task without resetting
 * any state. Use this when:
 * - Download failures need retry
 * - You want to resume downloads after a pause (e.g., exiting PICO-8)
 *
 * @see download_manager_rescan() when new content requires full rescan
 */
void download_manager_wake(void);

/**
 * @brief Reset cursors and rescan all channels from the beginning
 *
 * Resets download cursors to 0 and wakes the download task. This ensures
 * newly added index entries (which may appear before the current cursor
 * position) are discovered.
 *
 * USE THIS ONLY when new content has been added to the channel index:
 * - After channel refresh receives new index entries
 * - After async refresh completes with new content
 *
 * DO NOT use this for single file re-downloads or failures - use
 * download_manager_wake() instead.
 *
 * @see download_manager_wake() for lightweight wake without reset
 */
void download_manager_rescan(void);

/**
 * @brief Check if a download is currently in progress
 * 
 * @return true if downloading, false if idle
 */
bool download_manager_is_busy(void);

/**
 * @brief Get the channel ID of the currently active download (if any)
 *
 * @param out_channel_id Buffer to receive channel ID
 * @param max_len Size of buffer
 * @return true if there's an active download, false otherwise
 */
bool download_manager_get_active_channel(char *out_channel_id, size_t max_len);

/**
 * @brief Set the channel list for downloads (decoupled from Play Scheduler)
 *
 * Called by Play Scheduler after execute_playset() to configure which
 * channels the download manager should work on. Downloads use round-robin
 * across channels to find missing files.
 *
 * @param channel_ids Array of channel ID strings
 * @param count Number of channels
 */
void download_manager_set_channels(const char **channel_ids, size_t count);

/**
 * @brief Reset download cursors for all channels
 *
 * Called when channel cache is refreshed to rescan from the beginning.
 */
void download_manager_reset_cursors(void);

/**
 * @brief Build the local vault filepath for a channel cache entry
 *
 * Dispatches on the channel's type: Giphy entries map to the giphy cache,
 * institution entries to the per-museum tree (via the playset spec_name),
 * everything else to the sharded Makapix vault. Shared by the download
 * scan and the LAi verification sweep so both resolve identical paths.
 *
 * MUST only be called from the download manager task — the Makapix branch
 * uses single-task static path-building buffers.
 *
 * @param channel_id Channel the entry belongs to
 * @param entry      Cache entry (64-byte slot; format implied by channel)
 * @param out        Output buffer for the filepath
 * @param out_len    Output buffer size
 * @return ESP_OK on success;
 *         ESP_ERR_NOT_SUPPORTED for institution sentinel entries
 *         (0xFF unresolved / 0xFE tombstone — no file by design);
 *         ESP_ERR_NOT_FOUND if the channel's spec is no longer resolvable;
 *         ESP_FAIL / ESP_ERR_INVALID_ARG on path-build failure
 */
esp_err_t download_manager_build_entry_filepath(const char *channel_id,
                                                const makapix_channel_entry_t *entry,
                                                char *out, size_t out_len);

/**
 * @brief Check whether the in-flight download should bail out
 *
 * Set by `download_manager_set_channels` when the active playset changes
 * and the currently-busy channel is no longer in the new channel set.
 * The actual abort happens cooperatively: each downloader polls this
 * between chunks and returns `ESP_ERR_INVALID_STATE` when true (same
 * sentinel the USB-MSC export path uses).
 *
 * Thread-safe to call from any task. Cleared by the download task at
 * the start of each download iteration.
 *
 * @return true if the download task should abandon the current download.
 */
bool download_manager_is_canceled(void);

#ifdef __cplusplus
}
#endif

#endif // DOWNLOAD_MANAGER_H
