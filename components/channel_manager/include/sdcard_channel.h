#ifndef SDCARD_CHANNEL_H
#define SDCARD_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Asset type enumeration for supported animation formats
 * This is the canonical definition - other files should include this header
 */
typedef enum {
    ASSET_TYPE_WEBP,
    ASSET_TYPE_GIF,
    ASSET_TYPE_PNG,
    ASSET_TYPE_JPEG,
} asset_type_t;

#ifdef CONFIG_CHANNEL_MANAGER_MAX_POSTS
#define SDCARD_CHANNEL_MAX_POSTS CONFIG_CHANNEL_MANAGER_MAX_POSTS
#else
#define SDCARD_CHANNEL_MAX_POSTS 1000
#endif

#ifdef CONFIG_CHANNEL_MANAGER_PAGE_SIZE
#define SDCARD_CHANNEL_PAGE_SIZE CONFIG_CHANNEL_MANAGER_PAGE_SIZE
#else
#define SDCARD_CHANNEL_PAGE_SIZE 50
#endif

// Note: ANIMATIONS_DEFAULT_DIR is now deprecated.
// Use sd_path_get_animations() to get the current animations directory path.
// This macro is kept for Kconfig compatibility but should not be used in new code.
#ifdef CONFIG_CHANNEL_DEFAULT_ANIMATIONS_DIR
#define ANIMATIONS_DEFAULT_DIR CONFIG_CHANNEL_DEFAULT_ANIMATIONS_DIR
#else
// Note: Animations directory is now managed by sd_path module (configurable root)
// Use sd_path_get_animations() instead of this hardcoded path
#endif

/**
 * @brief Post structure representing a single artwork-type post
 */
typedef struct {
    char *name;           // Post name (filename without path)
    time_t created_at;    // File creation timestamp
    char *filepath;       // Full path for loading
    asset_type_t type;    // GIF, WebP, etc.
    uint32_t dwell_time_ms; // Effective dwell time for this item
    bool healthy;         // Load health flag
} sdcard_post_t;

/**
 * @brief Sort order for channel queries
 */
typedef enum {
    SDCARD_SORT_BY_NAME,  // Alphabetical by filename
    SDCARD_SORT_BY_DATE   // By creation date (newest first)
} sdcard_sort_order_t;

/**
 * @brief Query structure for paginated access
 */
typedef struct {
    size_t offset;                    // Starting offset
    size_t count;                     // Requested count (max 50)
    sdcard_sort_order_t sort_order;  // Desired sort order
} sdcard_query_t;

/**
 * @brief Query result structure
 */
typedef struct {
    sdcard_post_t *posts;  // Array of posts (caller must free)
    size_t count;          // Actual returned count
    size_t total;          // Total posts in channel
} sdcard_query_result_t;

/**
 * @brief Initialize the SD card channel
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sdcard_channel_init(void);

/**
 * @brief Deinitialize the SD card channel and free resources
 */
void sdcard_channel_deinit(void);

/**
 * @brief Refresh the channel by enumerating files from animations directory
 * 
 * Enumerates up to SDCARD_CHANNEL_MAX_POSTS files from the specified directory.
 * Only the first 1000 files found (in no particular order) are loaded.
 * 
 * @param animations_dir Directory path to scan (NULL uses default)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sdcard_channel_refresh(const char *animations_dir);

/**
 * @brief Query posts from the channel with pagination
 * 
 * Returns a page of posts sorted according to the query. The channel maintains
 * its internal sort order and only re-sorts if the requested order differs.
 * 
 * @param query Query parameters (offset, count, sort_order)
 * @param result Result structure (posts array will be allocated, caller must free)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sdcard_channel_query(const sdcard_query_t *query, sdcard_query_result_t *result);

/**
 * @brief Get total count of posts in the channel
 * 
 * @return Total post count (0 if channel not initialized)
 */
size_t sdcard_channel_get_total_count(void);

/**
 * @brief Get the creation date of the most recent post
 * 
 * @param out_date Pointer to receive the timestamp (NULL if no posts)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no posts
 */
esp_err_t sdcard_channel_get_latest_post_date(time_t *out_date);

/**
 * @brief Mark a post as unhealthy (failed to load)
 * 
 * @param post_index Index of the post to mark
 */
void sdcard_channel_mark_unhealthy(size_t post_index);

/**
 * @brief Get a post by index (for direct access after query)
 * 
 * @param post_index Index of the post
 * @param out_post Pointer to receive post pointer (NULL if invalid index)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid index
 */
esp_err_t sdcard_channel_get_post(size_t post_index, const sdcard_post_t **out_post);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_CHANNEL_H

