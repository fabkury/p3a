// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef CHANNEL_INTERFACE_H
#define CHANNEL_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "sdcard_channel.h"  // asset_type_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Channel order modes for playback
 */
typedef enum {
    CHANNEL_ORDER_ORIGINAL,  // Server/on-disk order
    CHANNEL_ORDER_CREATED,   // By creation date (newest first)
    CHANNEL_ORDER_RANDOM,    // Random shuffle (Fisher-Yates)
} channel_order_mode_t;

/**
 * @brief Filter flags for fast in-RAM filtering
 */
typedef uint16_t channel_filter_flags_t;

#define CHANNEL_FILTER_FLAG_NONE     0x0000
#define CHANNEL_FILTER_FLAG_NSFW     0x0001  // Item is NSFW
#define CHANNEL_FILTER_FLAG_GIF      0x0010  // Item is GIF format
#define CHANNEL_FILTER_FLAG_WEBP     0x0020  // Item is WebP format
#define CHANNEL_FILTER_FLAG_PNG      0x0040  // Item is PNG format
#define CHANNEL_FILTER_FLAG_JPEG     0x0080  // Item is JPEG format

/**
 * @brief Filter configuration for channel queries
 */
typedef struct {
    channel_filter_flags_t required_flags;   // Must have these flags
    channel_filter_flags_t excluded_flags;   // Must not have these flags
    // Future: add tag-based filtering via JSON lookup
} channel_filter_config_t;

/**
 * @brief Reference to a channel item for playback
 * 
 * Contains everything needed to load and display an artwork.
 */
typedef struct {
    int32_t post_id;          // Post ID for view tracking (0 if not a Makapix artwork)
    char filepath[256];       // Full path to the asset file
    char storage_key[96];     // Vault storage key (SHA256 hex prefix + extension)
    uint32_t item_index;      // Index within the channel
    channel_filter_flags_t flags;  // Cached filter flags
    uint32_t dwell_time_ms;   // Effective dwell time for this item (0 = default)
} channel_item_ref_t;

/**
 * @brief Channel post kind (high-level unit of content)
 *
 * A post can be either a single artwork or a playlist of artworks.
 */
typedef enum {
    CHANNEL_POST_KIND_ARTWORK = 0,
    CHANNEL_POST_KIND_PLAYLIST = 1,
} channel_post_kind_t;

/**
 * @brief Unified post representation for navigation (p/q)
 *
 * This is intentionally a "small" struct: playlist posts only carry identifiers
 * and counts. Playlist artwork metadata is expected to be loaded via
 * playlist_manager using playlist post_id.
 */
typedef struct {
    int32_t post_id;
    channel_post_kind_t kind;

    // Common fields
    uint32_t created_at;            // Unix timestamp (0 if unknown)
    time_t metadata_modified_at;    // 0 if unknown
    uint32_t dwell_time_ms;         // 0 = use channel default

    union {
        struct {
            char filepath[256];     // Direct path to file (SD card or vault)
            char storage_key[96];   // Vault storage key (UUID for Makapix, filename for SD)
            char art_url[256];      // Empty for SD card sources
            asset_type_t type;
            uint16_t width;
            uint16_t height;
            uint16_t frame_count;
            bool has_transparency;
            time_t artwork_modified_at; // 0 if unknown
        } artwork;

        struct {
            int32_t total_artworks;  // Server or local playlist size (0 if unknown)
        } playlist;
    } u;
} channel_post_t;

/**
 * @brief Channel statistics
 */
typedef struct {
    size_t total_items;      // Total items in channel
    size_t filtered_items;   // Items passing current filter
    size_t current_position; // Current playback position
} channel_stats_t;

/**
 * @brief Forward declaration of channel handle
 */
typedef struct channel_s *channel_handle_t;

/**
 * @brief Channel interface operations
 */
typedef struct {
    /**
     * @brief Load channel data into memory
     *
     * For SD card channel: scans directory for animation files
     * For Makapix channel: loads channel cache (.cache) from disk
     *
     * @param channel Channel handle
     * @return ESP_OK on success
     */
    esp_err_t (*load)(channel_handle_t channel);
    
    /**
     * @brief Unload channel data and free memory
     * 
     * @param channel Channel handle
     */
    void (*unload)(channel_handle_t channel);
    
    /**
     * @brief Start playback with specified order and filter
     * 
     * Builds the playback order array based on the mode and filter.
     * Resets position to the beginning.
     * 
     * @param channel Channel handle
     * @param order_mode Desired playback order
     * @param filter Filter configuration (can be NULL for no filtering)
     * @return ESP_OK on success
     */
    esp_err_t (*start_playback)(channel_handle_t channel, 
                                 channel_order_mode_t order_mode,
                                 const channel_filter_config_t *filter);
    
    /**
     * @brief Get the next item for playback
     * 
     * Advances position and returns the next item reference.
     * Wraps around at the end (re-shuffles if random mode).
     * 
     * @param channel Channel handle
     * @param out_item Pointer to receive item reference
     * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no items
     */
    esp_err_t (*next_item)(channel_handle_t channel, channel_item_ref_t *out_item);
    
    /**
     * @brief Get the previous item for playback
     * 
     * Moves position backward and returns the item reference.
     * Wraps around at the beginning.
     * 
     * @param channel Channel handle
     * @param out_item Pointer to receive item reference
     * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no items
     */
    esp_err_t (*prev_item)(channel_handle_t channel, channel_item_ref_t *out_item);
    
    /**
     * @brief Get the current item without advancing
     * 
     * @param channel Channel handle
     * @param out_item Pointer to receive item reference
     * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no items
     */
    esp_err_t (*current_item)(channel_handle_t channel, channel_item_ref_t *out_item);
    
    /**
     * @brief Request random reshuffle of playback order
     * 
     * Only effective if current order mode is CHANNEL_ORDER_RANDOM.
     * Preserves filter settings.
     * 
     * @param channel Channel handle
     * @return ESP_OK on success
     */
    esp_err_t (*request_reshuffle)(channel_handle_t channel);
    
    /**
     * @brief Request refresh from source
     * 
     * For SD card: re-scans directory
     * For Makapix: fetches updates via MQTT and updates index
     * 
     * This is an async operation for remote channels.
     * 
     * @param channel Channel handle
     * @return ESP_OK on success (operation started)
     */
    esp_err_t (*request_refresh)(channel_handle_t channel);
    
    /**
     * @brief Get channel statistics
     * 
     * @param channel Channel handle
     * @param out_stats Pointer to receive statistics
     * @return ESP_OK on success
     */
    esp_err_t (*get_stats)(channel_handle_t channel, channel_stats_t *out_stats);
    
    /**
     * @brief Destroy the channel and free all resources
     * 
     * @param channel Channel handle
     */
    void (*destroy)(channel_handle_t channel);

    // ---------------------------------------------------------------------
    // Optional post-level API (required for playlist support)
    // ---------------------------------------------------------------------
    size_t (*get_post_count)(channel_handle_t channel);
    esp_err_t (*get_post)(channel_handle_t channel, size_t post_index, channel_post_t *out_post);

    // ---------------------------------------------------------------------
    // Optional navigator API (used by Live Mode)
    // ---------------------------------------------------------------------
    // Returns an opaque pointer to the channel's play navigator (typically play_navigator_t*).
    // Can return NULL if navigator is not initialized/available for this channel.
    void *(*get_navigator)(channel_handle_t channel);
    
} channel_ops_t;

/**
 * @brief Base channel structure (all channels embed this)
 */
struct channel_s {
    const channel_ops_t *ops;  // Virtual function table
    char *name;                 // Channel name for display
    bool loaded;                // Channel data is loaded
    channel_order_mode_t current_order;
    channel_filter_config_t current_filter;
};

/**
 * @brief Convenience macros for calling channel operations
 */
#define channel_load(ch) ((ch)->ops->load(ch))
#define channel_unload(ch) ((ch)->ops->unload(ch))
#define channel_start_playback(ch, order, filter) ((ch)->ops->start_playback((ch), (order), (filter)))
#define channel_next_item(ch, out) ((ch)->ops->next_item((ch), (out)))
#define channel_prev_item(ch, out) ((ch)->ops->prev_item((ch), (out)))
#define channel_current_item(ch, out) ((ch)->ops->current_item((ch), (out)))
#define channel_request_reshuffle(ch) ((ch)->ops->request_reshuffle(ch))
#define channel_request_refresh(ch) ((ch)->ops->request_refresh(ch))
#define channel_get_stats(ch, out) ((ch)->ops->get_stats((ch), (out)))
#define channel_destroy(ch) ((ch)->ops->destroy(ch))

// Post-level helpers (optional ops)
static inline size_t channel_get_post_count(channel_handle_t ch)
{
    if (!ch || !ch->ops || !ch->ops->get_post_count) {
        return 0;
    }
    return ch->ops->get_post_count(ch);
}

static inline esp_err_t channel_get_post(channel_handle_t ch, size_t post_index, channel_post_t *out_post)
{
    if (!ch || !out_post) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ch->ops || !ch->ops->get_post) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ch->ops->get_post(ch, post_index, out_post);
}

static inline void *channel_get_navigator(channel_handle_t ch)
{
    if (!ch || !ch->ops || !ch->ops->get_navigator) {
        return NULL;
    }
    return ch->ops->get_navigator(ch);
}

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_INTERFACE_H

