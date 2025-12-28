/**
 * @file play_navigator.h
 * @brief Internal play navigator for channel implementations
 * 
 * NOTE: This is an internal implementation detail. External code should use
 * channel_player.h APIs (channel_player_swap_next, etc.) for navigation.
 */
#ifndef PLAY_NAVIGATOR_H
#define PLAY_NAVIGATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "channel_interface.h"
#include "playlist_manager.h"
#include "pcg32_reversible.h"

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
    char channel_id[64];
    play_order_mode_t order;       // Current play order
    uint32_t pe;                   // Playlist expansion (0 = infinite)
    bool randomize_playlist;       // Randomize within playlists
    bool live_mode;                // Live Mode synchronization
    uint32_t global_seed;          // Global seed
    
    uint32_t p;                    // Post index in channel
    uint32_t q;                    // In-playlist artwork index

    // Cached post order mapping
    uint32_t *order_indices;
    size_t order_count;

    // Channel-level dwell override (0 = disabled)
    uint32_t channel_dwell_override_ms;

    // Live mode flattened schedule
    bool live_ready;
    uint32_t live_count;
    uint32_t *live_p;
    uint32_t *live_q;
    
    // PCG32 PRNG for reversible random ordering
    pcg32_rng_t pcg_rng;
} play_navigator_t;

/**
 * @brief Initialize play navigator
 */
esp_err_t play_navigator_init(play_navigator_t *nav, channel_handle_t channel,
                              const char *channel_id,
                              play_order_mode_t order, uint32_t pe, uint32_t global_seed);

/**
 * @brief Deinitialize navigator and free resources
 */
void play_navigator_deinit(play_navigator_t *nav);

/**
 * @brief Get current artwork reference
 */
esp_err_t play_navigator_current(play_navigator_t *nav, artwork_ref_t *out_artwork);

/**
 * @brief Advance to next artwork
 */
esp_err_t play_navigator_next(play_navigator_t *nav, artwork_ref_t *out_artwork);

/**
 * @brief Go back to previous artwork
 */
esp_err_t play_navigator_prev(play_navigator_t *nav, artwork_ref_t *out_artwork);

/**
 * @brief Request a reshuffle (random order only)
 */
esp_err_t play_navigator_request_reshuffle(play_navigator_t *nav);

/**
 * @brief Jump to specific position
 */
esp_err_t play_navigator_jump(play_navigator_t *nav, uint32_t p, uint32_t q);

/**
 * @brief Validate current navigator state
 */
esp_err_t play_navigator_validate(play_navigator_t *nav);

/**
 * @brief Set playlist expansion
 */
void play_navigator_set_pe(play_navigator_t *nav, uint32_t pe);

/**
 * @brief Set play order mode
 */
void play_navigator_set_order(play_navigator_t *nav, play_order_mode_t order);

/**
 * @brief Set randomize playlist mode
 */
void play_navigator_set_randomize_playlist(play_navigator_t *nav, bool enable);

/**
 * @brief Set Live Mode
 */
void play_navigator_set_live_mode(play_navigator_t *nav, bool enable);

/**
 * @brief Set channel dwell override
 */
void play_navigator_set_channel_dwell_override_ms(play_navigator_t *nav, uint32_t dwell_ms);

/**
 * @brief Mark Live Mode schedule as dirty
 */
void play_navigator_mark_live_dirty(play_navigator_t *nav);

/**
 * @brief Get current p/q position
 */
void play_navigator_get_position(play_navigator_t *nav, uint32_t *out_p, uint32_t *out_q);

#ifdef __cplusplus
}
#endif

#endif // PLAY_NAVIGATOR_H

