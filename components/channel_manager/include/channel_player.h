#ifndef CHANNEL_PLAYER_H
#define CHANNEL_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdcard_channel.h"  // For sdcard_post_t and asset_type_t
#include "channel_interface.h"  // For channel_handle_t
#include "p3a_state.h"  // For p3a_channel_type_t

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_CHANNEL_MANAGER_MAX_POSTS
#define CHANNEL_PLAYER_MAX_POSTS CONFIG_CHANNEL_MANAGER_MAX_POSTS
#else
#define CHANNEL_PLAYER_MAX_POSTS 1000
#endif

/**
 * @brief Channel player source type
 */
typedef enum {
    CHANNEL_PLAYER_SOURCE_SDCARD,   // Using sdcard_channel
    CHANNEL_PLAYER_SOURCE_MAKAPIX,  // Using a Makapix channel_handle_t
} channel_player_source_t;

/**
 * @brief Swap request structure for validated artwork transitions
 * 
 * This structure contains all information needed for animation_player
 * to perform a seamless transition to a new artwork. All fields are
 * pre-validated by channel_player before being passed to animation_player.
 */
typedef struct swap_request_s {
    char filepath[256];        // Validated file path (file exists)
    asset_type_t type;         // File type (WebP, GIF, PNG, JPEG)
    int32_t post_id;           // For view tracking (0 if not applicable)
    uint32_t dwell_time_ms;    // Effective dwell time for this artwork
    uint64_t start_time_ms;    // For Live Mode alignment (0 = ignore)
    uint32_t start_frame;      // For Live Mode alignment (0 = start from beginning)
    bool is_live_mode;         // Whether this maintains Live Mode synchronization
} swap_request_t;

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
 * @brief Provide SD card channel handle (owned by channel_player)
 */
esp_err_t channel_player_set_sdcard_channel_handle(channel_handle_t sdcard_channel);

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
 * @brief Get the current item reference from the active channel
 * 
 * Returns the raw channel item with post_id, filepath, etc.
 * 
 * @param out_item Pointer to receive item reference
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no channel loaded
 */
esp_err_t channel_player_get_current_item(channel_item_ref_t *out_item);

/**
 * @brief Get the post_id of the current item (lightweight, stack-safe)
 * 
 * Use this instead of channel_player_get_current_item() when you only need
 * the post_id and want to minimize stack usage.
 * 
 * @param out_post_id Pointer to receive post_id (0 if not applicable)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no channel loaded
 */
esp_err_t channel_player_get_current_post_id(int32_t *out_post_id);

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

/**
 * @brief Switch to a Makapix channel as the playback source
 * 
 * After calling this, channel_player_get_current_post(), advance(), etc.
 * will use the provided Makapix channel instead of sdcard_channel.
 * 
 * @param makapix_channel Channel handle (must remain valid while in use)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t channel_player_switch_to_makapix_channel(channel_handle_t makapix_channel);

/**
 * @brief Switch back to SD card channel as the playback source
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t channel_player_switch_to_sdcard_channel(void);

/**
 * @brief Get current playback source type
 * 
 * @return Current source type (SDCARD or MAKAPIX)
 */
channel_player_source_t channel_player_get_source_type(void);

/**
 * @brief Check if Live Mode is currently active
 * 
 * @return true if Live Mode is enabled and active, false otherwise
 */
bool channel_player_is_live_mode_active(void);

/**
 * @brief Get the current channel's navigator (opaque pointer)
 *
 * @return Navigator pointer (typically play_navigator_t*), or NULL if unavailable.
 */
void *channel_player_get_navigator(void);

/**
 * @brief Exit Live Mode if currently active
 * 
 * Exits Live Mode by disabling it in the navigator and canceling
 * any pending swap_futures. Should be called on manual swaps.
 */
void channel_player_exit_live_mode(void);

/**
 * @brief Clear the current channel pointer if it matches the given channel
 * 
 * This should be called BEFORE destroying a channel to prevent race conditions
 * where other tasks might try to access the freed channel.
 * 
 * @param channel_to_clear Channel handle that is about to be destroyed
 */
void channel_player_clear_channel(channel_handle_t channel_to_clear);

/**
 * @brief Hot-swap the play order while preserving current position
 * 
 * Changes the play order mode for the current channel without restarting playback.
 * The current artwork remains displayed, and navigation continues from there
 * in the new order.
 * 
 * @param play_order Play order: 1=CREATED (date descending), 2=RANDOM
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no channel active
 */
esp_err_t channel_player_set_play_order(uint8_t play_order);

// ============================================================================
// NEW NAVIGATION API (Phase 1 Refactor)
// ============================================================================

/**
 * @brief Request navigation to next artwork
 * 
 * Returns immediately. If a command is already being processed, this call
 * is ignored and returns ESP_ERR_INVALID_STATE.
 * 
 * @return ESP_OK if command accepted
 *         ESP_ERR_INVALID_STATE if another command is in progress
 */
esp_err_t channel_player_swap_next(void);

/**
 * @brief Request navigation to previous artwork
 * 
 * Returns immediately. If a command is already being processed, this call
 * is ignored and returns ESP_ERR_INVALID_STATE.
 * 
 * @return ESP_OK if command accepted
 *         ESP_ERR_INVALID_STATE if another command is in progress
 */
esp_err_t channel_player_swap_back(void);

/**
 * @brief Request navigation to a specific position and swap to it
 * 
 * Sets the playback position to (p, q) and initiates a swap to that artwork.
 * If the artwork at that position isn't available, scans forward to find
 * the first available artwork.
 * 
 * @param p Post index in the play order
 * @param q Artwork index within a playlist (0 for single artworks)
 * @return ESP_OK if command accepted and swap initiated
 *         ESP_ERR_INVALID_STATE if another command is in progress
 *         ESP_ERR_NOT_FOUND if no available artwork found
 */
esp_err_t channel_player_swap_to(uint32_t p, uint32_t q);

/**
 * @brief Check if a navigation command is currently being processed
 * 
 * @return true if command active, false if idle
 */
bool channel_player_is_command_active(void);

/**
 * @brief Switch to a different channel
 * 
 * Immediately marks the new channel as active and attempts to display
 * the first available artwork. If no artworks are available, displays
 * an appropriate message but still considers the switch successful.
 * 
 * @param type Channel type (SDCARD, MAKAPIX_ALL, etc.)
 * @param identifier Additional identifier (user_sqid, hashtag, etc.) or NULL
 * @return ESP_OK on success (even if channel is empty)
 *         ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t channel_player_switch_channel(p3a_channel_type_t type, const char *identifier);

/**
 * @brief Set global dwell time override
 * 
 * @param seconds Dwell time in seconds (0 = disable override)
 * @return ESP_OK on success
 */
esp_err_t channel_player_set_dwell_time(uint32_t seconds);

/**
 * @brief Get current dwell time setting
 * 
 * @return Dwell time in seconds
 */
uint32_t channel_player_get_dwell_time(void);

/**
 * @brief Enter Live Mode synchronized playback
 * 
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if preconditions not met (e.g., NTP not synced)
 */
esp_err_t channel_player_enter_live_mode(void);

/**
 * @brief Notification from animation_player that swap succeeded
 * 
 * Called by animation_player after successfully swapping to new artwork.
 * Used for Live Mode scheduling and dwell timer reset.
 */
void channel_player_notify_swap_succeeded(void);

/**
 * @brief Notification from animation_player that swap failed
 * 
 * Called by animation_player if swap fails. Used for Live Mode recovery.
 * 
 * @param error The error code from the failed swap
 */
void channel_player_notify_swap_failed(esp_err_t error);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_PLAYER_H

