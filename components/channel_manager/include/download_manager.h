#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "playlist_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOWNLOAD_PRIORITY_HIGH = 0,
    DOWNLOAD_PRIORITY_MEDIUM = 1,
    DOWNLOAD_PRIORITY_LOW = 2,
} download_priority_t;

/**
 * @brief Callback invoked when a download completes successfully
 * 
 * @param channel_id The channel ID this download was for (may be empty)
 * @param storage_key The storage key of the downloaded artwork
 * @param filepath The local filepath where the artwork was saved
 * @param user_ctx User context passed to download_manager_set_completion_callback
 */
typedef void (*download_completion_cb_t)(const char *channel_id, const char *storage_key, 
                                          const char *filepath, void *user_ctx);

typedef struct {
    int32_t playlist_post_id;     // 0 if not from a playlist
    int32_t artwork_post_id;      // Artwork post_id (if known)
    char storage_key[96];
    char art_url[256];
    char filepath[256];           // Expected local path (vault). May be empty.
    download_priority_t priority;
    char channel_id[64];          // Channel this download belongs to (for cancellation)
} download_request_t;

esp_err_t download_manager_init(void);
void download_manager_deinit(void);

esp_err_t download_queue(const download_request_t *req);

/**
 * @brief Convenience: enqueue a playlist artwork for download
 *
 * @param channel_id Channel ID this artwork belongs to (used for cancellation). Can be NULL/empty.
 */
esp_err_t download_queue_artwork(const char *channel_id,
                                 int32_t playlist_post_id,
                                 const artwork_ref_t *artwork,
                                 download_priority_t priority);

/**
 * @brief Cancel all pending downloads for a specific channel
 * 
 * This clears queued requests for the channel. The currently in-progress download
 * (if any) will complete, but its result may be ignored.
 * 
 * @param channel_id Channel ID to cancel downloads for
 */
void download_manager_cancel_channel(const char *channel_id);

/**
 * @brief Set the completion callback for downloads
 * 
 * @param cb Callback function (can be NULL to disable)
 * @param user_ctx User context passed to callback
 */
void download_manager_set_completion_callback(download_completion_cb_t cb, void *user_ctx);

/**
 * @brief Get the channel ID of the currently active download (if any)
 * 
 * @param out_channel_id Buffer to receive channel ID
 * @param max_len Size of buffer
 * @return true if there's an active download, false otherwise
 */
bool download_manager_get_active_channel(char *out_channel_id, size_t max_len);

/**
 * @brief Clear the cancelled state for a channel
 * 
 * Call this before starting new downloads for a channel that was previously cancelled.
 * 
 * @param channel_id Channel ID to clear (or NULL to clear any)
 */
void download_manager_clear_cancelled(const char *channel_id);

/**
 * @brief Get the number of pending downloads in the queue
 * 
 * @return Number of items currently in the download queue
 */
size_t download_manager_get_pending_count(void);

/**
 * @brief Get the available space in the download queue
 * 
 * @return Number of additional items that can be queued
 */
size_t download_manager_get_queue_space(void);

/**
 * @brief Wait until the download queue has space for at least min_space items
 * 
 * @param min_space Minimum number of free slots to wait for
 * @param timeout_ms Maximum time to wait in milliseconds (0 = no timeout)
 * @return true if space is available, false if timed out
 */
bool download_manager_wait_for_space(size_t min_space, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // DOWNLOAD_MANAGER_H

