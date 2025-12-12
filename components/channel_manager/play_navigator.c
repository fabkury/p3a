#include "play_navigator.h"
#include "playlist_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "play_nav";

// PCG random state for random play order
typedef struct {
    uint64_t state;
    uint64_t inc;
} pcg32_random_t;

// PCG32 random functions (simple, reversible)
static uint32_t pcg32_random_r(pcg32_random_t *rng)
{
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static void pcg32_srandom_r(pcg32_random_t *rng, uint64_t initstate, uint64_t initseq)
{
    rng->state = 0U;
    rng->inc = (initseq << 1u) | 1u;
    pcg32_random_r(rng);
    rng->state += initstate;
    pcg32_random_r(rng);
}

// Forward declarations
static esp_err_t get_post_at_index(play_navigator_t *nav, uint32_t index, 
                                    channel_item_ref_t *out_item);
static uint32_t get_effective_playlist_size(playlist_metadata_t *playlist, uint32_t pe);

esp_err_t play_navigator_init(play_navigator_t *nav, channel_handle_t channel,
                               play_order_mode_t order, uint32_t pe)
{
    if (!nav || !channel) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(nav, 0, sizeof(play_navigator_t));
    
    nav->channel = channel;
    nav->order = order;
    nav->pe = pe;
    nav->randomize_playlist = false;
    nav->live_mode = false;
    nav->p = 0;
    nav->q = 0;
    
    // Initialize random state if needed
    if (order == PLAY_ORDER_RANDOM) {
        pcg32_random_t *rng = (pcg32_random_t *)malloc(sizeof(pcg32_random_t));
        if (!rng) {
            return ESP_ERR_NO_MEM;
        }
        
        // TODO: Get channel ID to generate seed
        uint32_t channel_id = 0;  // Placeholder
        nav->channel_seed = 0xFAB ^ ((uint64_t)channel_id);
        
        pcg32_srandom_r(rng, nav->channel_seed, 0);
        nav->random_state = rng;
    }
    
    ESP_LOGI(TAG, "Navigator initialized: order=%d, pe=%lu", order, pe);
    return ESP_OK;
}

void play_navigator_deinit(play_navigator_t *nav)
{
    if (!nav) {
        return;
    }
    
    if (nav->random_state) {
        free(nav->random_state);
        nav->random_state = NULL;
    }
}

esp_err_t play_navigator_current(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav || !out_artwork) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate state first
    play_navigator_validate(nav);
    
    // Get post at current p
    channel_item_ref_t item;
    esp_err_t err = get_post_at_index(nav, nav->p, &item);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get post at index %lu", nav->p);
        return err;
    }
    
    // Check if it's a playlist (simplified - we'll need to check post kind)
    // For now, assume it's an artwork post
    // TODO: Check if post is playlist type
    
    memset(out_artwork, 0, sizeof(artwork_ref_t));
    strncpy(out_artwork->storage_key, item.storage_key, sizeof(out_artwork->storage_key) - 1);
    
    // TODO: Fill in other artwork fields from item
    
    return ESP_OK;
}

esp_err_t play_navigator_next(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate current state
    play_navigator_validate(nav);
    
    // Get channel stats to know total post count
    channel_stats_t stats;
    esp_err_t err = channel_get_stats(nav->channel, &stats);
    if (err != ESP_OK || stats.total_items == 0) {
        ESP_LOGE(TAG, "Failed to get channel stats or no items");
        return ESP_ERR_NOT_FOUND;
    }
    
    // TODO: Check if current post is a playlist
    // For now, assume all posts are artworks (simplified)
    bool current_is_playlist = false;
    
    if (current_is_playlist) {
        // TODO: Get playlist and check if we can advance within it
        playlist_metadata_t *playlist = NULL;
        // ... playlist navigation logic ...
        
        // For now, just advance to next post
        nav->p = (nav->p + 1) % stats.total_items;
        nav->q = 0;
    } else {
        // Simple case: advance to next post
        nav->p = (nav->p + 1) % stats.total_items;
        nav->q = 0;
    }
    
    ESP_LOGD(TAG, "Advanced to p=%lu, q=%lu", nav->p, nav->q);
    
    // Get new current artwork if requested
    if (out_artwork) {
        return play_navigator_current(nav, out_artwork);
    }
    
    return ESP_OK;
}

esp_err_t play_navigator_prev(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate current state
    play_navigator_validate(nav);
    
    // Get channel stats
    channel_stats_t stats;
    esp_err_t err = channel_get_stats(nav->channel, &stats);
    if (err != ESP_OK || stats.total_items == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // If we're at q > 0, move back within current playlist
    if (nav->q > 0) {
        nav->q--;
    } else {
        // Move to previous post
        if (nav->p == 0) {
            nav->p = stats.total_items - 1;
        } else {
            nav->p--;
        }
        
        // TODO: Check if previous post is a playlist
        // If so, set q to last artwork in that playlist
        nav->q = 0;
    }
    
    ESP_LOGD(TAG, "Moved back to p=%lu, q=%lu", nav->p, nav->q);
    
    if (out_artwork) {
        return play_navigator_current(nav, out_artwork);
    }
    
    return ESP_OK;
}

esp_err_t play_navigator_jump(play_navigator_t *nav, uint32_t p, uint32_t q)
{
    if (!nav) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate position
    channel_stats_t stats;
    esp_err_t err = channel_get_stats(nav->channel, &stats);
    if (err != ESP_OK) {
        return err;
    }
    
    if (p >= stats.total_items) {
        ESP_LOGE(TAG, "Invalid p=%lu (max=%zu)", p, stats.total_items);
        return ESP_ERR_INVALID_ARG;
    }
    
    nav->p = p;
    nav->q = q;
    
    // Validate will check if q is valid for this post
    return play_navigator_validate(nav);
}

esp_err_t play_navigator_validate(play_navigator_t *nav)
{
    if (!nav || !nav->channel) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get channel stats
    channel_stats_t stats;
    esp_err_t err = channel_get_stats(nav->channel, &stats);
    if (err != ESP_OK) {
        return err;
    }
    
    // Check p is valid
    if (nav->p >= stats.total_items) {
        ESP_LOGE(TAG, "Invalid p=%lu (max=%zu), resetting to 0", nav->p, stats.total_items);
        nav->p = 0;
        nav->q = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    // TODO: Check if current post is a playlist and validate q
    // For now, assume q=0 is always valid
    if (nav->q > 0) {
        ESP_LOGW(TAG, "q=%lu but post may not be a playlist, resetting to 0", nav->q);
        nav->q = 0;
    }
    
    return ESP_OK;
}

void play_navigator_set_pe(play_navigator_t *nav, uint32_t pe)
{
    if (!nav) {
        return;
    }
    
    if (pe > 1023) {
        ESP_LOGW(TAG, "PE %lu > 1023, clamping", pe);
        pe = 1023;
    }
    
    nav->pe = pe;
    ESP_LOGI(TAG, "Set PE to %lu", pe);
}

void play_navigator_set_order(play_navigator_t *nav, play_order_mode_t order)
{
    if (!nav) {
        return;
    }
    
    nav->order = order;
    
    // Initialize/cleanup random state as needed
    if (order == PLAY_ORDER_RANDOM && !nav->random_state) {
        pcg32_random_t *rng = (pcg32_random_t *)malloc(sizeof(pcg32_random_t));
        if (rng) {
            uint32_t channel_id = 0;  // TODO: Get from channel
            nav->channel_seed = 0xFAB ^ ((uint64_t)channel_id);
            pcg32_srandom_r(rng, nav->channel_seed, 0);
            nav->random_state = rng;
        }
    } else if (order != PLAY_ORDER_RANDOM && nav->random_state) {
        free(nav->random_state);
        nav->random_state = NULL;
    }
    
    ESP_LOGI(TAG, "Set play order to %d", order);
}

void play_navigator_set_randomize_playlist(play_navigator_t *nav, bool enable)
{
    if (!nav) {
        return;
    }
    
    nav->randomize_playlist = enable;
    ESP_LOGI(TAG, "Set randomize_playlist to %s", enable ? "ON" : "OFF");
}

void play_navigator_set_live_mode(play_navigator_t *nav, bool enable)
{
    if (!nav) {
        return;
    }
    
    nav->live_mode = enable;
    ESP_LOGI(TAG, "Set live_mode to %s", enable ? "ON" : "OFF");
}

void play_navigator_get_position(play_navigator_t *nav, uint32_t *out_p, uint32_t *out_q)
{
    if (!nav) {
        return;
    }
    
    if (out_p) {
        *out_p = nav->p;
    }
    if (out_q) {
        *out_q = nav->q;
    }
}

// Helper functions

static esp_err_t get_post_at_index(play_navigator_t *nav, uint32_t index,
                                    channel_item_ref_t *out_item)
{
    if (!nav || !out_item) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // TODO: Implement proper post access via channel interface
    // This will depend on play order mode
    
    return channel_current_item(nav->channel, out_item);
}

static uint32_t get_effective_playlist_size(playlist_metadata_t *playlist, uint32_t pe)
{
    if (!playlist) {
        return 0;
    }
    
    uint32_t max_size = (pe == 0) ? (uint32_t)playlist->total_artworks : pe;
    return (max_size < (uint32_t)playlist->available_artworks) ? 
           max_size : (uint32_t)playlist->available_artworks;
}
