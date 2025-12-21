#include "play_navigator.h"
#include "esp_log.h"
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

static uint64_t wall_clock_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// Helper to get the effective seed for random operations
static uint32_t get_effective_seed(play_navigator_t *nav)
{
    // Use effective seed (true random pre-NTP, deterministic post-NTP)
    uint32_t effective = config_store_get_effective_seed();
    // XOR with navigator's global seed for per-navigator variation if needed
    return effective ^ nav->global_seed;
}

// Fisher-Yates shuffle using PCG32 for deterministic random ordering
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
        // Return OK with empty order - channel can still be used once posts arrive
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

        // Simple bubble-sort (post_count is typically small; keeps code size small)
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
        // Seed PCG32 with effective seed and shuffle
        uint64_t seed = (uint64_t)get_effective_seed(nav);
        uint64_t stream = (uint64_t)nav->global_seed;  // Use global_seed as stream selector
        pcg32_seed(&nav->pcg_rng, seed, stream);
        shuffle_indices(indices, post_count, &nav->pcg_rng);
    } else {
        // PLAY_ORDER_SERVER: keep sequential
    }

    free(nav->order_indices);
    nav->order_indices = indices;
    nav->order_count = post_count;

    // Clamp p/q
    if (nav->p >= nav->order_count) {
        nav->p = 0;
        nav->q = 0;
    }

    return ESP_OK;
}

static esp_err_t get_current_post(play_navigator_t *nav, channel_post_t *out_post)
{
    if (!nav || !out_post) return ESP_ERR_INVALID_ARG;
    if (!nav->order_indices || nav->order_count == 0) {
        esp_err_t err = rebuild_order(nav);
        if (err != ESP_OK) return err;
    }
    if (nav->p >= nav->order_count) return ESP_ERR_INVALID_STATE;
    size_t post_idx = (size_t)nav->order_indices[nav->p];
    return channel_get_post(nav->channel, post_idx, out_post);
}

static uint32_t get_effective_playlist_size(const playlist_metadata_t *playlist, uint32_t pe)
{
    if (!playlist) return 0;
    // Effective size is based on loaded artworks; missing items are skipped dynamically.
    uint32_t count = (uint32_t)playlist->loaded_artworks;
    if (pe == 0) return count;
    return (count < pe) ? count : pe;
}

static uint32_t playlist_map_q_to_index(play_navigator_t *nav, int32_t playlist_post_id, uint32_t effective_size, uint32_t q)
{
    if (!nav || effective_size == 0) return 0;
    if (!nav->randomize_playlist) return q;
    
    // Per spec: effective_seed ^ playlist_post_id ^ playlist_post_q_index
    // Use PCG32 for deterministic per-playlist randomization
    uint64_t seed = (uint64_t)(get_effective_seed(nav) ^ (uint32_t)playlist_post_id);
    uint64_t stream = (uint64_t)q;  // q as stream selector for independent per-q randomness
    pcg32_rng_t temp_rng;
    pcg32_seed(&temp_rng, seed, stream);
    uint32_t r = pcg32_next_u32(&temp_rng);
    return r % effective_size;
}

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                           uint32_t channel_override_ms,
                                           uint32_t playlist_or_artwork_ms)
{
    uint32_t eff = playlist_or_artwork_ms;
    if (channel_override_ms != 0) eff = channel_override_ms;
    if (global_override_ms != 0) eff = global_override_ms;
    if (eff == 0) eff = 30000;
    return eff;
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

static esp_err_t build_live_schedule(play_navigator_t *nav)
{
    if (!nav) return ESP_ERR_INVALID_ARG;

    free_live_schedule(nav);

    if (!nav->channel || nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    // Count flattened items
    uint32_t total = 0;
    for (size_t i = 0; i < nav->order_count; i++) {
        channel_post_t post = {0};
        if (channel_get_post(nav->channel, nav->order_indices[i], &post) != ESP_OK) continue;
        if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
            playlist_metadata_t *pl = NULL;
            if (playlist_get(post.post_id, nav->pe, &pl) == ESP_OK && pl) {
                total += get_effective_playlist_size(pl, nav->pe);
            }
        } else {
            total += 1;
        }
    }

    if (total == 0) return ESP_ERR_NOT_FOUND;

    nav->live_p = (uint32_t *)calloc(total, sizeof(uint32_t));
    nav->live_q = (uint32_t *)calloc(total, sizeof(uint32_t));
    if (!nav->live_p || !nav->live_q) {
        free_live_schedule(nav);
        return ESP_ERR_NO_MEM;
    }

    animation_t *anims = (animation_t *)calloc(total, sizeof(animation_t));
    if (!anims) {
        free_live_schedule(nav);
        return ESP_ERR_NO_MEM;
    }

    uint32_t global_override_ms = config_store_get_dwell_time();
    uint32_t idx = 0;
    const uint64_t channel_epoch_ms = live_mode_get_channel_start_time(nav->channel) * 1000ULL;
    uint64_t seg_cursor_ms = 0; // virtual time from channel epoch within one channel cycle

    for (size_t i = 0; i < nav->order_count && idx < total; i++) {
        channel_post_t post = {0};
        if (channel_get_post(nav->channel, nav->order_indices[i], &post) != ESP_OK) continue;

        if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
            playlist_metadata_t *pl = NULL;
            if (playlist_get(post.post_id, nav->pe, &pl) != ESP_OK || !pl) continue;

            uint32_t effective = get_effective_playlist_size(pl, nav->pe);
            if (effective == 0) {
                continue;
            }
            if ((uint32_t)pl->loaded_artworks < effective) {
                // For Live Mode, we require a complete deterministic schedule across devices.
                ESP_LOGW(TAG, "Live schedule: playlist %ld metadata incomplete (loaded=%ld < effective=%u)",
                         (long)post.post_id, (long)pl->loaded_artworks, (unsigned)effective);
                free(anims);
                free_live_schedule(nav);
                return ESP_ERR_INVALID_STATE;
            }

            uint32_t *dwell_by_q = (uint32_t *)calloc(effective, sizeof(uint32_t));
            if (!dwell_by_q) {
                free(anims);
                free_live_schedule(nav);
                return ESP_ERR_NO_MEM;
            }

            uint64_t playlist_cycle_ms = 0;
            for (uint32_t q = 0; q < effective; q++) {
                uint32_t art_idx = playlist_map_q_to_index(nav, post.post_id, effective, q);
                if (art_idx >= (uint32_t)pl->loaded_artworks) {
                    // Should not happen due to loaded_artworks >= effective check.
                    dwell_by_q[q] = 30000;
                } else {
                    const artwork_ref_t *a = &pl->artworks[art_idx];
                    uint32_t dwell = (pl->dwell_time_ms != 0) ? pl->dwell_time_ms : a->dwell_time_ms;
                    dwell_by_q[q] = compute_effective_dwell_ms(global_override_ms,
                                                               nav->channel_dwell_override_ms,
                                                               dwell);
                }
                playlist_cycle_ms += (uint64_t)dwell_by_q[q];
            }
            if (playlist_cycle_ms == 0) {
                playlist_cycle_ms = (uint64_t)effective * 30000ULL;
            }

            // Align the playlist segment to the playlist's own creation time (post.created_at).
            // Compute which q should be active when the channel reaches this playlist segment in virtual time.
            const uint64_t playlist_start_ms = live_mode_get_playlist_start_time(post.created_at) * 1000ULL;
            const uint64_t seg_start_ms = channel_epoch_ms + seg_cursor_ms;
            uint64_t rel_ms = 0;
            if (seg_start_ms > playlist_start_ms) {
                rel_ms = seg_start_ms - playlist_start_ms;
            }
            uint64_t phase_ms = rel_ms % playlist_cycle_ms;

            uint32_t start_q = 0;
            for (uint32_t q = 0; q < effective; q++) {
                uint32_t d = dwell_by_q[q] ? dwell_by_q[q] : 30000;
                if (phase_ms < (uint64_t)d) {
                    start_q = q;
                    break;
                }
                phase_ms -= (uint64_t)d;
            }

            for (uint32_t k = 0; k < effective && idx < total; k++) {
                uint32_t q = (start_q + k) % effective;
                uint32_t art_idx = playlist_map_q_to_index(nav, post.post_id, effective, q);
                if (art_idx >= (uint32_t)pl->loaded_artworks) continue;

                anims[idx].duration_ms = dwell_by_q[q] ? dwell_by_q[q] : 30000;
                nav->live_p[idx] = (uint32_t)i;
                nav->live_q[idx] = q;
                idx++;
            }

            seg_cursor_ms += playlist_cycle_ms;
            free(dwell_by_q);
        } else {
            anims[idx].duration_ms = compute_effective_dwell_ms(global_override_ms,
                                                                nav->channel_dwell_override_ms,
                                                                post.dwell_time_ms);
            nav->live_p[idx] = (uint32_t)i;
            nav->live_q[idx] = 0;
            idx++;
            seg_cursor_ms += (uint64_t)anims[idx - 1].duration_ms;
        }
    }

    nav->live_count = idx;
    if (nav->live_count == 0) {
        free(anims);
        free_live_schedule(nav);
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t start_ms = live_mode_get_channel_start_time(nav->channel) * 1000ULL;
    SyncPlaylist.init((uint64_t)get_effective_seed(nav), start_ms, anims, nav->live_count, SYNC_MODE_PRECISE);
    SyncPlaylist.enable_live(true);
    free(anims);

    nav->live_ready = true;
    return ESP_OK;
}

esp_err_t play_navigator_init(play_navigator_t *nav, channel_handle_t channel,
                              const char *channel_id,
                              play_order_mode_t order, uint32_t pe, uint32_t global_seed)
{
    if (!nav || !channel) return ESP_ERR_INVALID_ARG;
    memset(nav, 0, sizeof(*nav));
    nav->channel = channel;
    if (channel_id && channel_id[0]) {
        strlcpy(nav->channel_id, channel_id, sizeof(nav->channel_id));
    } else {
        nav->channel_id[0] = '\0';
    }
    nav->order = order;
    nav->pe = pe;
    nav->global_seed = global_seed ? global_seed : 0xFAB;
    nav->randomize_playlist = false;
    nav->live_mode = false;
    nav->channel_dwell_override_ms = 0;
    nav->live_ready = false;
    nav->live_count = 0;
    nav->live_p = NULL;
    nav->live_q = NULL;
    nav->p = 0;
    nav->q = 0;

    return rebuild_order(nav);
}

void play_navigator_deinit(play_navigator_t *nav)
{
    if (!nav) return;
    free(nav->order_indices);
    nav->order_indices = NULL;
    nav->order_count = 0;
    free_live_schedule(nav);
}

esp_err_t play_navigator_validate(play_navigator_t *nav)
{
    if (!nav || !nav->channel) return ESP_ERR_INVALID_STATE;
    if (!nav->order_indices || nav->order_count == 0) {
        esp_err_t err = rebuild_order(nav);
        if (err != ESP_OK) return err;
    }

    if (nav->p >= nav->order_count) {
        nav->p = 0;
        nav->q = 0;
        return ESP_ERR_INVALID_STATE;
    }

    channel_post_t post = {0};
    esp_err_t err = get_current_post(nav, &post);
    if (err != ESP_OK) {
        nav->p = 0;
        nav->q = 0;
        return ESP_ERR_INVALID_STATE;
    }

    if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *pl = NULL;
        if (playlist_get(post.post_id, nav->pe, &pl) != ESP_OK || !pl) {
            nav->q = 0;
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t effective = get_effective_playlist_size(pl, nav->pe);
        if (effective == 0) {
            nav->q = 0;
            return ESP_OK;
        }
        if (nav->q >= effective) {
            nav->q = 0;
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        // Artwork post
        if (nav->q != 0) {
            nav->q = 0;
            return ESP_ERR_INVALID_STATE;
        }
    }

    return ESP_OK;
}

static void prefetch_playlist_lookahead(play_navigator_t *nav,
                                        int32_t playlist_post_id,
                                        playlist_metadata_t *pl,
                                        uint32_t effective,
                                        uint32_t start_q,
                                        uint32_t target_buffer)
{
    // Downloads are handled automatically by download_manager's single-download approach
    // This function is now a no-op; the download_manager scans from current position
    (void)nav;
    (void)playlist_post_id;
    (void)pl;
    (void)effective;
    (void)start_q;
    (void)target_buffer;
}

esp_err_t play_navigator_current(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav || !out_artwork) return ESP_ERR_INVALID_ARG;
    (void)play_navigator_validate(nav);

    if (nav->live_mode) {
        if (!nav->live_ready) {
            esp_err_t e = build_live_schedule(nav);
            if (e != ESP_OK) return e;
        }

        if (!sntp_sync_is_synchronized()) {
            ESP_LOGW(TAG, "Live Mode enabled but SNTP not synchronized");
        }

        uint32_t cur_idx = 0;
        uint32_t elapsed_ms = 0;
        (void)SyncPlaylist.update(wall_clock_ms(), &cur_idx, &elapsed_ms);
        if (cur_idx >= nav->live_count) return ESP_ERR_NOT_FOUND;

        nav->p = nav->live_p[cur_idx];
        nav->q = nav->live_q[cur_idx];
    }

    channel_post_t post = {0};
    esp_err_t err = get_current_post(nav, &post);
    if (err != ESP_OK) return err;

    memset(out_artwork, 0, sizeof(*out_artwork));

    if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *pl = NULL;
        err = playlist_get(post.post_id, nav->pe, &pl);
        if (err != ESP_OK || !pl) return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;

        uint32_t effective = get_effective_playlist_size(pl, nav->pe);
        if (effective == 0) return ESP_ERR_NOT_FOUND;

        // Skip unavailable items deterministically by scanning forward within playlist.
        uint32_t q = nav->q;
        for (uint32_t tries = 0; tries < effective; tries++) {
            uint32_t idx = playlist_map_q_to_index(nav, post.post_id, effective, q);
            if (idx < (uint32_t)pl->loaded_artworks) {
                const artwork_ref_t *cand = &pl->artworks[idx];
                if (cand->downloaded && file_exists(cand->filepath)) {
                    nav->q = q;
                    *out_artwork = *cand;
                    // Playlist-level dwell overrides artwork dwell (if non-zero).
                    if (pl->dwell_time_ms != 0) {
                        out_artwork->dwell_time_ms = pl->dwell_time_ms;
                    }
                    // Keep a small play buffer warm (best-effort, non-blocking).
                    prefetch_playlist_lookahead(nav, post.post_id, pl, effective, nav->q, 6);
                    return ESP_OK;
                }
            }
            q = (q + 1) % effective;
        }
        return ESP_ERR_NOT_FOUND;
    }

    // Artwork post
    out_artwork->post_id = post.post_id;
    strlcpy(out_artwork->filepath, post.u.artwork.filepath, sizeof(out_artwork->filepath));
    strlcpy(out_artwork->storage_key, post.u.artwork.storage_key, sizeof(out_artwork->storage_key));
    strlcpy(out_artwork->art_url, post.u.artwork.art_url, sizeof(out_artwork->art_url));
    out_artwork->type = post.u.artwork.type;
    out_artwork->dwell_time_ms = post.dwell_time_ms;
    out_artwork->width = post.u.artwork.width;
    out_artwork->height = post.u.artwork.height;
    out_artwork->frame_count = post.u.artwork.frame_count;
    out_artwork->has_transparency = post.u.artwork.has_transparency;
    out_artwork->metadata_modified_at = post.metadata_modified_at;
    out_artwork->artwork_modified_at = post.u.artwork.artwork_modified_at;
    out_artwork->downloaded = file_exists(out_artwork->filepath);
    return ESP_OK;
}

esp_err_t play_navigator_next(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    (void)play_navigator_validate(nav);

    if (nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    if (nav->live_mode) {
        if (!nav->live_ready) {
            esp_err_t e = build_live_schedule(nav);
            if (e != ESP_OK) return e;
        }
        SyncPlaylist.next();
        return out_artwork ? play_navigator_current(nav, out_artwork) : ESP_OK;
    }

    // Skip holes without stalling: try up to one full cycle.
    for (size_t tries = 0; tries < nav->order_count + 1; tries++) {
        channel_post_t post = {0};
        if (get_current_post(nav, &post) != ESP_OK) return ESP_ERR_NOT_FOUND;

        if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
            playlist_metadata_t *pl = NULL;
            if (playlist_get(post.post_id, nav->pe, &pl) == ESP_OK && pl) {
                uint32_t effective = get_effective_playlist_size(pl, nav->pe);
                if (effective > 0) {
                    // Move within playlist first (q++)
                    if ((nav->q + 1) < effective) {
                        nav->q++;
                    } else {
                        // Exit playlist
                        nav->p = (nav->p + 1) % (uint32_t)nav->order_count;
                        nav->q = 0;
                    }
                } else {
                    // Empty playlist: move to next post
                    nav->p = (nav->p + 1) % (uint32_t)nav->order_count;
                    nav->q = 0;
                }
            } else {
                // Playlist not available yet: move to next post
                nav->p = (nav->p + 1) % (uint32_t)nav->order_count;
                nav->q = 0;
            }
        } else {
            // Artwork post: advance to next post
            nav->p = (nav->p + 1) % (uint32_t)nav->order_count;
            nav->q = 0;
        }

        if (!out_artwork) return ESP_OK;
        if (play_navigator_current(nav, out_artwork) == ESP_OK) {
            return ESP_OK;
        }
        // else: keep skipping forward
    }

    return ESP_ERR_NOT_FOUND;
}

// Helper: find the last available artwork in the channel (for wrap-around)
// Returns true if found, sets nav->p and nav->q to the position
static bool find_last_available_artwork(play_navigator_t *nav)
{
    if (!nav || nav->order_count == 0) return false;
    
    // Scan backwards from the last position to find an available artwork
    for (size_t i = nav->order_count; i > 0; i--) {
        uint32_t p = (uint32_t)(i - 1);
        channel_post_t post = {0};
        if (channel_get_post(nav->channel, nav->order_indices[p], &post) != ESP_OK) continue;
        
        if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
            playlist_metadata_t *pl = NULL;
            if (playlist_get(post.post_id, nav->pe, &pl) != ESP_OK || !pl) continue;
            
            uint32_t effective = get_effective_playlist_size(pl, nav->pe);
            if (effective == 0) continue;
            
            // Scan playlist backwards for available artwork
            for (uint32_t k = effective; k > 0; k--) {
                uint32_t q = k - 1;
                uint32_t idx = playlist_map_q_to_index(nav, post.post_id, effective, q);
                if (idx < (uint32_t)pl->loaded_artworks) {
                    const artwork_ref_t *cand = &pl->artworks[idx];
                    if (cand->downloaded && file_exists(cand->filepath)) {
                        nav->p = p;
                        nav->q = q;
                        return true;
                    }
                }
            }
        } else {
            // Artwork post - check if file exists
            if (file_exists(post.u.artwork.filepath)) {
                nav->p = p;
                nav->q = 0;
                return true;
            }
        }
    }
    
    return false;
}

// Helper: try to get artwork at current position, scanning BACKWARDS if unavailable
static esp_err_t get_current_artwork_scan_backwards(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav || !out_artwork) return ESP_ERR_INVALID_ARG;
    
    channel_post_t post = {0};
    esp_err_t err = get_current_post(nav, &post);
    if (err != ESP_OK) return err;
    
    memset(out_artwork, 0, sizeof(*out_artwork));
    
    if (post.kind == CHANNEL_POST_KIND_PLAYLIST) {
        playlist_metadata_t *pl = NULL;
        err = playlist_get(post.post_id, nav->pe, &pl);
        if (err != ESP_OK || !pl) return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;
        
        uint32_t effective = get_effective_playlist_size(pl, nav->pe);
        if (effective == 0) return ESP_ERR_NOT_FOUND;
        
        // Scan BACKWARDS within playlist to find available artwork
        uint32_t q = nav->q;
        for (uint32_t tries = 0; tries < effective; tries++) {
            uint32_t idx = playlist_map_q_to_index(nav, post.post_id, effective, q);
            if (idx < (uint32_t)pl->loaded_artworks) {
                const artwork_ref_t *cand = &pl->artworks[idx];
                if (cand->downloaded && file_exists(cand->filepath)) {
                    nav->q = q;
                    *out_artwork = *cand;
                    if (pl->dwell_time_ms != 0) {
                        out_artwork->dwell_time_ms = pl->dwell_time_ms;
                    }
                    return ESP_OK;
                }
            }
            // Move backwards in playlist (with wrap-around)
            if (q == 0) q = effective - 1;
            else q--;
        }
        return ESP_ERR_NOT_FOUND;
    }
    
    // Artwork post - check if file exists
    if (!file_exists(post.u.artwork.filepath)) {
        return ESP_ERR_NOT_FOUND;
    }
    
    out_artwork->post_id = post.post_id;
    strlcpy(out_artwork->filepath, post.u.artwork.filepath, sizeof(out_artwork->filepath));
    strlcpy(out_artwork->storage_key, post.u.artwork.storage_key, sizeof(out_artwork->storage_key));
    strlcpy(out_artwork->art_url, post.u.artwork.art_url, sizeof(out_artwork->art_url));
    out_artwork->type = post.u.artwork.type;
    out_artwork->dwell_time_ms = post.dwell_time_ms;
    out_artwork->width = post.u.artwork.width;
    out_artwork->height = post.u.artwork.height;
    out_artwork->frame_count = post.u.artwork.frame_count;
    out_artwork->has_transparency = post.u.artwork.has_transparency;
    out_artwork->metadata_modified_at = post.metadata_modified_at;
    out_artwork->artwork_modified_at = post.u.artwork.artwork_modified_at;
    out_artwork->downloaded = true;
    return ESP_OK;
}

esp_err_t play_navigator_prev(play_navigator_t *nav, artwork_ref_t *out_artwork)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    (void)play_navigator_validate(nav);

    if (nav->order_count == 0) return ESP_ERR_NOT_FOUND;

    if (nav->live_mode) {
        if (!nav->live_ready) {
            esp_err_t e = build_live_schedule(nav);
            if (e != ESP_OK) return e;
        }
        SyncPlaylist.prev();
        return out_artwork ? play_navigator_current(nav, out_artwork) : ESP_OK;
    }

    // Track starting position to detect full cycle
    uint32_t start_p = nav->p;
    uint32_t start_q = nav->q;
    bool wrapped = false;

    for (size_t tries = 0; tries < nav->order_count * 2 + 1; tries++) {
        channel_post_t post = {0};
        if (get_current_post(nav, &post) != ESP_OK) {
            // Move back regardless
            if (nav->p == 0) {
                nav->p = (uint32_t)nav->order_count - 1;
                wrapped = true;
            } else {
                nav->p--;
            }
            nav->q = 0;
            continue;
        }

        if (post.kind == CHANNEL_POST_KIND_PLAYLIST && nav->q > 0) {
            nav->q--;
        } else {
            // Move to previous post
            if (nav->p == 0) {
                // Wrap around to the last position in the channel
                nav->p = (uint32_t)nav->order_count - 1;
                wrapped = true;
            } else {
                nav->p--;
            }
            nav->q = 0;

            // If previous post is a playlist, go to its last item
            channel_post_t prev_post = {0};
            if (get_current_post(nav, &prev_post) == ESP_OK && prev_post.kind == CHANNEL_POST_KIND_PLAYLIST) {
                playlist_metadata_t *pl = NULL;
                if (playlist_get(prev_post.post_id, nav->pe, &pl) == ESP_OK && pl) {
                    uint32_t effective = get_effective_playlist_size(pl, nav->pe);
                    if (effective > 0) {
                        nav->q = effective - 1;
                    }
                }
            }
        }

        // Check if we've cycled back to start
        if (wrapped && nav->p == start_p && nav->q == start_q) {
            break;
        }

        if (!out_artwork) return ESP_OK;
        
        // Use backwards-scanning version to find available artwork
        if (get_current_artwork_scan_backwards(nav, out_artwork) == ESP_OK) {
            return ESP_OK;
        }
        // Keep going backwards to find an available artwork
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t play_navigator_jump(play_navigator_t *nav, uint32_t p, uint32_t q)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    if (!nav->order_indices || nav->order_count == 0) {
        esp_err_t err = rebuild_order(nav);
        if (err != ESP_OK) return err;
    }
    if (p >= nav->order_count) return ESP_ERR_INVALID_ARG;
    nav->p = p;
    nav->q = q;
    return play_navigator_validate(nav);
}

void play_navigator_set_pe(play_navigator_t *nav, uint32_t pe)
{
    if (!nav) return;
    if (pe > 1023) pe = 1023;
    nav->pe = pe;
    if (nav->live_mode) free_live_schedule(nav);
}

void play_navigator_set_order(play_navigator_t *nav, play_order_mode_t order)
{
    if (!nav) return;
    nav->order = order;
    (void)rebuild_order(nav);
    if (nav->live_mode) free_live_schedule(nav);
}

void play_navigator_set_randomize_playlist(play_navigator_t *nav, bool enable)
{
    if (!nav) return;
    nav->randomize_playlist = enable;
    if (nav->live_mode) free_live_schedule(nav);
}

void play_navigator_set_live_mode(play_navigator_t *nav, bool enable)
{
    if (!nav) return;
    nav->live_mode = enable;
    if (!enable) {
        SyncPlaylist.enable_live(false);
        free_live_schedule(nav);
    } else {
        SyncPlaylist.enable_live(true);
        // rebuild on demand
        free_live_schedule(nav);
    }
}

void play_navigator_set_channel_dwell_override_ms(play_navigator_t *nav, uint32_t dwell_ms)
{
    if (!nav) return;
    nav->channel_dwell_override_ms = dwell_ms;
    if (nav->live_mode) {
        free_live_schedule(nav);
    }
}

void play_navigator_mark_live_dirty(play_navigator_t *nav)
{
    if (!nav) return;
    // Do not free here (may be called from other tasks). The schedule will be rebuilt
    // by the playback task the next time play_navigator_current() runs in Live Mode.
    nav->live_ready = false;
}

esp_err_t play_navigator_request_reshuffle(play_navigator_t *nav)
{
    if (!nav) return ESP_ERR_INVALID_ARG;
    if (nav->order != PLAY_ORDER_RANDOM) return ESP_OK;
    if (nav->live_mode) free_live_schedule(nav);
    return rebuild_order(nav);
}

void play_navigator_get_position(play_navigator_t *nav, uint32_t *out_p, uint32_t *out_q)
{
    if (!nav) return;
    if (out_p) *out_p = nav->p;
    if (out_q) *out_q = nav->q;
}
