#ifndef CHANNEL_PLAYER_H
#define CHANNEL_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdcard_channel.h"  // For sdcard_post_t and asset_type_t

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_CHANNEL_MANAGER_MAX_POSTS
#define CHANNEL_PLAYER_MAX_POSTS CONFIG_CHANNEL_MANAGER_MAX_POSTS
#else
#define CHANNEL_PLAYER_MAX_POSTS 1000
#endif

/**
 * @brief Channel player state structure
 */
typedef struct {
    sdcard_post_t *posts;  // Array of loaded posts (up to 1000)
    size_t *indices;       // Playback order (indices into posts array)
    size_t count;          // Number of loaded posts
    size_t current_pos;   // Current position in playback order
    bool randomize;       // Randomization enabled (default: false)
} channel_player_state_t;

/**
 * @brief Initialize the channel player
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t channel_player_init(void);

/**
 * @brief Deinitialize the channel player and free resources
 */
void channel_player_deinit(void);

/**
 * @brief Load posts from the channel for playback
 * 
 * Loads up to CHANNEL_PLAYER_MAX_POSTS most recent posts from the channel
 * using paginated queries. Posts are sorted by creation date (newest first).
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t channel_player_load_channel(void);

/**
 * @brief Get the current post for playback
 * 
 * @param out_post Pointer to receive the current post (NULL if no posts loaded)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no posts loaded
 */
esp_err_t channel_player_get_current_post(const sdcard_post_t **out_post);

/**
 * @brief Advance to the next post
 * 
 * If at the end of the list and randomization is enabled, re-randomizes
 * the list and wraps to the beginning. Otherwise, just wraps to the beginning.
 * 
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no posts loaded
 */
esp_err_t channel_player_advance(void);

/**
 * @brief Go back to the previous post
 * 
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no posts loaded
 */
esp_err_t channel_player_go_back(void);

/**
 * @brief Set randomization mode
 * 
 * @param enable True to enable randomization, false to disable
 */
void channel_player_set_randomize(bool enable);

/**
 * @brief Check if randomization is enabled
 * 
 * @return True if randomization is enabled, false otherwise
 */
bool channel_player_is_randomized(void);

/**
 * @brief Get current playback position
 * 
 * @return Current position index (0-based), or SIZE_MAX if no posts loaded
 */
size_t channel_player_get_current_position(void);

/**
 * @brief Get total number of loaded posts
 * 
 * @return Number of loaded posts, or 0 if none loaded
 */
size_t channel_player_get_post_count(void);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_PLAYER_H

