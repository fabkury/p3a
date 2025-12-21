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
 * @brief Signal that downloads may be needed
 * 
 * Wakes the download task to check for new files to download.
 * Call this after:
 * - Channel refresh completes
 * - A download completes (automatically called internally)
 * - Navigator position changes significantly
 */
void download_manager_signal_work_available(void);

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

#ifdef __cplusplus
}
#endif

#endif // DOWNLOAD_MANAGER_H
