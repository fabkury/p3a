#ifndef PLAY_NAVIGATOR_H
#define PLAY_NAVIGATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "channel_interface.h"
#include "playlist_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Play order modes
 */
typedef enum {
    PLAY_ORDER_SERVER = 0,   // Server/original order
    PLAY_ORDER_CREATED = 1,  // By creation date (newest first)
    PLAY_ORDER_RANDOM = 2,   // Random shuffle with seed
} play_order_mode_t;

/**
 * @brief Play navigator state
 * 
 * Manages p/q indices for navigating through posts and playlists.
 * Does NOT own the channel handle.
 */
typedef struct {
    channel_handle_t channel;      // Channel being navigated (not owned)
    play_order_mode_t order;       // Current play order
    uint32_t pe;                   // Playlist expansion (0 = infinite)
    bool randomize_playlist;       // Randomize within playlists
    bool live_mode;                // Live Mode synchronization
    
    uint32_t p;                    // Post index in channel
    uint32_t q;                    // In-playlist artwork index (0 if not in playlist)
    
    uint32_t channel_seed;         // For random mode
    void *random_state;            // Opaque pointer to PCG state (for random mode)
} play_navigator_t;

/**
 * @brief Initialize play navigator
 * 
 * @param nav Navigator structure to initialize
 * @param channel Channel to navigate (must remain valid while navigator is used)
 * @param order Initial play order
 * @param pe Playlist expansion
 * @return ESP_OK on success
 */
esp_err_t play_navigator_init(play_navigator_t *nav, channel_handle_t channel, 
                               play_order_mode_t order, uint32_t pe);

/**
 * @brief Deinitialize navigator and free resources
 * 
 * @param nav Navigator to deinitialize
 */
void play_navigator_deinit(play_navigator_t *nav);

/**
 * @brief Get current artwork reference
 * 
 * Returns artwork at current p/q position.
 * 
 * @param nav Navigator
 * @param out_artwork Pointer to receive artwork reference (do NOT free)
 * @return ESP_OK on success
 */
esp_err_t play_navigator_current(play_navigator_t *nav, artwork_ref_t *out_artwork);

/**
 * @brief Advance to next artwork in play queue
 * 
 * Updates p and q according to play order and playlist expansion.
 * 
 * @param nav Navigator
 * @param out_artwork Pointer to receive next artwork reference (do NOT free, can be NULL)
 * @return ESP_OK on success
 */
esp_err_t play_navigator_next(play_navigator_t *nav, artwork_ref_t *out_artwork);

/**
 * @brief Go back to previous artwork in play queue
 * 
 * Updates p and q. Fully reversible in all modes.
 * 
 * @param nav Navigator
 * @param out_artwork Pointer to receive previous artwork reference (do NOT free, can be NULL)
 * @return ESP_OK on success
 */
esp_err_t play_navigator_prev(play_navigator_t *nav, artwork_ref_t *out_artwork);

/**
 * @brief Jump to specific post/artwork position
 * 
 * @param nav Navigator
 * @param p Post index
 * @param q In-playlist artwork index (0 if not in playlist)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if position invalid
 */
esp_err_t play_navigator_jump(play_navigator_t *nav, uint32_t p, uint32_t q);

/**
 * @brief Validate current navigator state
 * 
 * Checks if p/q point to valid positions. Resets to (0,0) if invalid.
 * 
 * @param nav Navigator
 * @return ESP_OK if valid, ESP_ERR_INVALID_STATE if had to reset
 */
esp_err_t play_navigator_validate(play_navigator_t *nav);

/**
 * @brief Set playlist expansion
 * 
 * @param nav Navigator
 * @param pe New playlist expansion (0-1023)
 */
void play_navigator_set_pe(play_navigator_t *nav, uint32_t pe);

/**
 * @brief Set play order mode
 * 
 * @param nav Navigator
 * @param order New play order
 */
void play_navigator_set_order(play_navigator_t *nav, play_order_mode_t order);

/**
 * @brief Set randomize playlist mode
 * 
 * @param nav Navigator
 * @param enable True to randomize playlists internally
 */
void play_navigator_set_randomize_playlist(play_navigator_t *nav, bool enable);

/**
 * @brief Set Live Mode
 * 
 * @param nav Navigator
 * @param enable True to enable Live Mode sync
 */
void play_navigator_set_live_mode(play_navigator_t *nav, bool enable);

/**
 * @brief Get current p/q position
 * 
 * @param nav Navigator
 * @param out_p Pointer to receive post index (can be NULL)
 * @param out_q Pointer to receive in-playlist index (can be NULL)
 */
void play_navigator_get_position(play_navigator_t *nav, uint32_t *out_p, uint32_t *out_q);

#ifdef __cplusplus
}
#endif

#endif // PLAY_NAVIGATOR_H
