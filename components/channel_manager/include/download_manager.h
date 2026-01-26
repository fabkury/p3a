// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "playlist_manager.h"

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
 * - A single file needs re-download (e.g., after eviction from LAi)
 * - Download failures need retry (LTF backoff controls timing)
 * - You want to wake the task without losing scan progress
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
 * Called by Play Scheduler after execute_command() to configure which
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
 * @brief Reset the playback_initiated flag
 *
 * Called when changing channels to allow the download manager to trigger
 * initial playback when the first file becomes available for the new channel.
 */
void download_manager_reset_playback_initiated(void);

#ifdef __cplusplus
}
#endif

#endif // DOWNLOAD_MANAGER_H
