/**
 * @file play_navigator.c
 * @brief Internal play navigator implementation for channel navigation
 * 
 * This module manages p/q indices for navigating through posts and playlists.
 * It is used internally by channel implementations (sdcard, makapix).
 * 
 * External code should use channel_player.h APIs for navigation.
 */

#include "play_navigator.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sync_playlist.h"
#include "sntp_sync.h"
#include "config_store.h"
#include "download_manager.h"
#include "live_mode.h"
#include "pcg32_reversible.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "play_navigator";

// Helper to get the effective seed for random operations
static uint32_t get_effective_seed(play_navigator_t *nav)
{
    uint32_t effective = config_store_get_effective_seed();
    return effective ^ nav->global_seed;
}

// Fisher-Yates shuffle using PCG32
static void shuffle_indices(uint32_t *indices, size_t count, pcg32_rng_t *rng)
{
    if (!indices || count <= 1 || !rng) return;
    
    for (size_t i = count - 1; i > 0; i--) {
        uint32_t r = pcg32_next_u32(rng);
        size_t j = (size_t)(r % (uint32_t)(i + 1));
        uint32_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }
}

static esp_err_t rebuild_order(play_navigator_t *nav)
{
    if (!nav || !nav->channel) return ESP_ERR_INVALID_ARG;

    size_t post_count = channel_get_post_count(nav->channel);
    if (post_count == 0) {
        free(nav->order_indices);
        nav->order_indices = NULL;
        nav->order_count = 0;
        nav->p = 0;
        nav->q = 0;
        return ESP_OK;
    }

    uint32_t *indices = (uint32_t *)malloc(post_count * sizeof(uint32_t));
    if (!indices) return ESP_ERR_NO_MEM;
    
    for (size_t i = 0; i < post_count; i++) {
        indices[i] = (uint32_t)i;
    }

    if (nav->order == PLAY_ORDER_CREATED) {
        typedef struct {
            uint32_t idx;
            uint32_t created_at;
        } sort_item_t;

        sort_item_t *items = (sort_item_t *)malloc(post_count * sizeof(sort_item_t));
        if (!items) {
            free(indices);
            return ESP_ERR_NO_MEM;
        }

        for (size_t i = 0; i < post_count; i++) {
            channel_post_t post = {0};
            uint32_t created = 0;
            if (channel_get_post(nav->channel, i, &post) == ESP_OK) {
                created = post.created_at;
            }
            items[i].idx = (uint32_t)i;
            items[i].created_at = created;
        }

        // Bubble sort (post_count is typically small)
        for (size_t i = 0; i + 1 < post_count; i++) {
            for (size_t j = 0; j + 1 < post_count - i; j++) {
                if (items[j].created_at < items[j + 1].created_at) {
                    sort_item_t tmp = items[j];
                    items[j] = items[j + 1];
                    items[j + 1] = tmp;
                }
            }
        }

        for (size_t i = 0; i < post_count; i++) {
            indices[i] = items[i].idx;
        }
        free(items);
        
    } else if (nav->order == PLAY_ORDER_RANDOM) {
        uint32_t seed = get_effective_seed(nav);
        pcg32_seed(&nav->pcg_rng, seed, 0);
        shuffle_indices(indices, post_count, &nav->pcg_rng);
    }

    free(nav->order_indices);
    nav->order_indices = indices;
    nav->order_count = post_count;

    return ESP_OK;
}

static bool file_exists(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static esp_err_t get_current_post(play_navigator_t *nav, channel_post_t *out_post)
{
    if (!nav || !out_post) return ESP_ERR_INVALID_ARG;
    if (nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    uint32_t idx = nav->order_indices[nav->p % nav->order_count];
    return channel_get_post(nav->channel, idx, out_post);
}

static size_t get_effective_playlist_size(play_navigator_t *nav, playlist_metadata_t *playlist)
{
    if (!playlist) return 0;
    size_t count = (size_t)playlist->loaded_artworks;
    if (nav->pe == 0) return count;  // Infinite
    return (nav->pe < count) ? nav->pe : count;
}

static uint32_t playlist_map_q_to_index(play_navigator_t *nav, int32_t playlist_post_id, 
                                        uint32_t effective_size, uint32_t q)
{
    if (!nav || effective_size == 0) return 0;
    
    if (nav->randomize_playlist) {
        uint32_t seed = get_effective_seed(nav) ^ (uint32_t)playlist_post_id;
        pcg32_rng_t rng;
        pcg32_seed(&rng, seed, 0);
        
        uint32_t *playlist_indices = (uint32_t *)malloc(effective_size * sizeof(uint32_t));
        if (!playlist_indices) return q % effective_size;
        
        for (uint32_t i = 0; i < effective_size; i++) {
            playlist_indices[i] = i;
        }
        shuffle_indices(playlist_indices, effective_size, &rng);
        
        uint32_t mapped = playlist_indices[q % effective_size];
        free(playlist_indices);
        return mapped;
    }
    
    return q % effective_size;
}

static void free_live_schedule(play_navigator_t *nav)
{
    if (!nav) return;
    free(nav->live_p);
    free(nav->live_q);
    nav->live_p = NULL;
    nav->live_q = NULL;
    nav->live_count = 0;
    nav->live_ready = false;
}

esp_err_t play_navigator_init(play_navigator_t *nav, channel_handle_t channel,
                              const char *channel_id,
                              play_order_mode_t order, uint32_t pe, uint32_t global_seed)
{
    if (!nav) return ESP_ERR_INVALID_ARG;

    memset(nav, 0, sizeof(play_navigator_t));
    nav->channel = channel;
    if (channel_id) {
        strlcpy(nav->channel_id, channel_id, sizeof(nav->channel_id));
    }
    nav->order = order;
    nav->pe = pe;
    nav->global_seed = global_seed;
    nav->randomize_playlist = false;
    nav->live_mode = false;
    nav->p = 0;
    nav->q = 0;

    return rebuild_order(nav);
}

void play_navigator_deinit(play_navigator_t *nav)
{
    if (!nav) return;
    free(nav->order_indices);
    free_live_schedule(nav);
    memset(nav, 0, sizeof(play_navigator_t));
}

esp_err_t play_navigator_validate(play_navigator_t *nav)
{
    if (!nav || !nav->channel) return ESP_ERR_INVALID_STATE;
    
    size_t post_count = channel_get_post_count(nav->channel);
    
    // Rebuild if post count changed
    if (nav->order_count != post_count) {
        esp_err_t err = rebuild_order(nav);
        if (err != ESP_OK) return err;
    }
    
    if (nav->order_count == 0) {
        nav->p = 0;
        nav->q = 0;
        return ESP_OK;
    }
    
    // Clamp p to valid range
    if (nav->p >= nav->order_count) {
        nav->p = 0;
        nav->q = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    return ESP_OK;
}

esp_err_t play_navigator_current(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav || !out_artwork) return ESP_ERR_INVALID_ARG;
    (void)play_navigator_validate(nav);

    if (nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    channel_post_t post = {0};
    esp_err_t err = get_current_post(nav, &post);
    if (err != ESP_OK) return err;

    // Check if it's a playlist
    if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *playlist = NULL;
        err = playlist_get(post.post_id, nav->pe, &playlist);
        if (err != ESP_OK || !playlist || playlist->loaded_artworks == 0) {
            if (playlist) playlist_release(playlist);
            return ESP_ERR_NOT_FOUND;
        }

        size_t effective = get_effective_playlist_size(nav, playlist);
        uint32_t mapped_q = playlist_map_q_to_index(nav, post.post_id, effective, nav->q);
        
        if (mapped_q >= (uint32_t)playlist->loaded_artworks) {
            playlist_release(playlist);
            return ESP_ERR_INVALID_STATE;
        }

        artwork_ref_t *art = &playlist->artworks[mapped_q];
        memcpy(out_artwork, art, sizeof(artwork_ref_t));
        
        if (nav->channel_dwell_override_ms > 0) {
            out_artwork->dwell_time_ms = nav->channel_dwell_override_ms;
        }

        playlist_release(playlist);
        return ESP_OK;
    }

    // Single artwork post
    memset(out_artwork, 0, sizeof(artwork_ref_t));
    strlcpy(out_artwork->filepath, post.u.artwork.filepath, sizeof(out_artwork->filepath));
    out_artwork->post_id = post.post_id;
    out_artwork->dwell_time_ms = (nav->channel_dwell_override_ms > 0) ? 
                                  nav->channel_dwell_override_ms : post.dwell_time_ms;
    out_artwork->downloaded = file_exists(post.u.artwork.filepath);

    return ESP_OK;
}

esp_err_t play_navigator_next(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    (void)play_navigator_validate(nav);

    if (nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    // Check if current post is a playlist
    channel_post_t post = {0};
    esp_err_t err = get_current_post(nav, &post);
    
    if (err == ESP_OK && post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *playlist = NULL;
        err = playlist_get(post.post_id, nav->pe, &playlist);
        if (err == ESP_OK && playlist) {
            size_t effective = get_effective_playlist_size(nav, playlist);
            if (effective > 0 && nav->q + 1 < effective) {
                nav->q++;
                playlist_release(playlist);
                if (out_artwork) {
                    return play_navigator_current(nav, out_artwork);
                }
                return ESP_OK;
            }
            playlist_release(playlist);
        }
    }

    // Advance to next post
    nav->q = 0;
    nav->p++;
    
    if (nav->p >= nav->order_count) {
        nav->p = 0;
        if (nav->order == PLAY_ORDER_RANDOM) {
            rebuild_order(nav);
        }
    }

    if (out_artwork) {
        return play_navigator_current(nav, out_artwork);
    }
    return ESP_OK;
}

esp_err_t play_navigator_prev(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    (void)play_navigator_validate(nav);

    if (nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    if (nav->q > 0) {
        nav->q--;
        if (out_artwork) {
            return play_navigator_current(nav, out_artwork);
        }
        return ESP_OK;
    }

    // Go to previous post
    if (nav->p == 0) {
        nav->p = (nav->order_count > 0) ? (nav->order_count - 1) : 0;
    } else {
        nav->p--;
    }

    // If new post is a playlist, jump to its last artwork
    channel_post_t post = {0};
    esp_err_t err = get_current_post(nav, &post);
    
    if (err == ESP_OK && post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *playlist = NULL;
        err = playlist_get(post.post_id, nav->pe, &playlist);
        if (err == ESP_OK && playlist) {
            size_t effective = get_effective_playlist_size(nav, playlist);
            nav->q = (effective > 0) ? (effective - 1) : 0;
            playlist_release(playlist);
        }
    } else {
        nav->q = 0;
    }

    if (out_artwork) {
        return play_navigator_current(nav, out_artwork);
    }
    return ESP_OK;
}

esp_err_t play_navigator_request_reshuffle(play_navigator_t *nav)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    if (nav->order != PLAY_ORDER_RANDOM) return ESP_OK;
    
    return rebuild_order(nav);
}

esp_err_t play_navigator_jump(play_navigator_t *nav, uint32_t p, uint32_t q)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    if (p >= nav->order_count) return ESP_ERR_INVALID_ARG;
    
    nav->p = p;
    nav->q = q;
    return ESP_OK;
}

void play_navigator_set_pe(play_navigator_t *nav, uint32_t pe)
{
    if (!nav) return;
    nav->pe = pe;
    free_live_schedule(nav);
}

void play_navigator_set_order(play_navigator_t *nav, play_order_mode_t order)
{
    if (!nav) return;
    nav->order = order;
    rebuild_order(nav);
    free_live_schedule(nav);
}

void play_navigator_set_randomize_playlist(play_navigator_t *nav, bool enable)
{
    if (!nav) return;
    nav->randomize_playlist = enable;
    free_live_schedule(nav);
}

void play_navigator_set_live_mode(play_navigator_t *nav, bool enable)
{
    if (!nav) return;
    nav->live_mode = enable;
    if (!enable) {
        free_live_schedule(nav);
    }
}

void play_navigator_set_channel_dwell_override_ms(play_navigator_t *nav, uint32_t dwell_ms)
{
    if (!nav) return;
    nav->channel_dwell_override_ms = dwell_ms;
    free_live_schedule(nav);
}

void play_navigator_mark_live_dirty(play_navigator_t *nav)
{
    if (!nav) return;
    nav->live_ready = false;
}

void play_navigator_get_position(play_navigator_t *nav, uint32_t *out_p, uint32_t *out_q)
{
    if (!nav) return;
    if (out_p) *out_p = nav->p;
    if (out_q) *out_q = nav->q;
}

