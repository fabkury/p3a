// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "makapix_channel_internal.h"
#include "makapix_api.h"
#include "makapix_artwork.h"
#include "play_navigator.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "channel_settings.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "makapix_channel";

// Current channel for download callback (only one channel can be active for downloads)
static makapix_channel_t *s_download_channel = NULL;

// Weak symbol for SD pause check
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

// Forward declarations
static esp_err_t makapix_impl_load(channel_handle_t channel);
static void makapix_impl_unload(channel_handle_t channel);
static esp_err_t makapix_impl_start_playback(channel_handle_t channel, 
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter);
static esp_err_t makapix_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t makapix_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t makapix_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t makapix_impl_request_reshuffle(channel_handle_t channel);
static esp_err_t makapix_impl_request_refresh(channel_handle_t channel);
static esp_err_t makapix_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats);
static void makapix_impl_destroy(channel_handle_t channel);
static size_t makapix_impl_get_post_count(channel_handle_t channel);
static esp_err_t makapix_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post);
static void *makapix_impl_get_navigator(channel_handle_t channel);

// Virtual function table
static const channel_ops_t s_makapix_ops = {
    .load = makapix_impl_load,
    .unload = makapix_impl_unload,
    .start_playback = makapix_impl_start_playback,
    .next_item = makapix_impl_next_item,
    .prev_item = makapix_impl_prev_item,
    .current_item = makapix_impl_current_item,
    .request_reshuffle = makapix_impl_request_reshuffle,
    .request_refresh = makapix_impl_request_refresh,
    .get_stats = makapix_impl_get_stats,
    .destroy = makapix_impl_destroy,
    .get_post_count = makapix_impl_get_post_count,
    .get_post = makapix_impl_get_post,
    .get_navigator = makapix_impl_get_navigator,
};

// Helper: get filter flags from entry
static channel_filter_flags_t get_entry_flags(const makapix_channel_entry_t *entry)
{
    channel_filter_flags_t flags = entry->filter_flags;
    
    // Add format flags based on extension (artwork posts only)
    if (entry->kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
        switch (entry->extension) {
            case EXT_GIF:  flags |= CHANNEL_FILTER_FLAG_GIF; break;
            case EXT_WEBP: flags |= CHANNEL_FILTER_FLAG_WEBP; break;
            case EXT_PNG:  flags |= CHANNEL_FILTER_FLAG_PNG; break;
            case EXT_JPEG: flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        }
    }
    
    return flags;
}

// Interface implementation

static esp_err_t makapix_impl_load(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    if (ch->base.loaded) {
        ESP_LOGW(TAG, "Channel already loaded");
        makapix_impl_unload(channel);
    }
    
    char index_path[256];
    build_index_path(ch, index_path, sizeof(index_path));

    // Recover/cleanup channel index before attempting to load
    if (ch->index_io_lock) {
        xSemaphoreTake(ch->index_io_lock, portMAX_DELAY);
    }
    makapix_index_recover_and_cleanup(index_path);
    
    ESP_LOGD(TAG, "Loading channel from: %s", index_path);
    
    // Try to open index file
    FILE *f = fopen(index_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Index file not found: %s (errno=%d)", index_path, errno);
        ch->entries = NULL;
        ch->entry_count = 0;
        ch->base.loaded = true;
        
        // Start refresh to fetch channel data from server
        if (!ch->refreshing) {
            ESP_LOGD(TAG, "Starting refresh to populate empty channel");
            esp_err_t refresh_err = makapix_impl_request_refresh(channel);
            if (refresh_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start refresh for empty channel");
                if (ch->index_io_lock) {
                    xSemaphoreGive(ch->index_io_lock);
                }
                return refresh_err;
            }
        }
        if (ch->index_io_lock) {
            xSemaphoreGive(ch->index_io_lock);
        }
        return ESP_OK;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size % sizeof(makapix_channel_entry_t) != 0) {
        ESP_LOGW(TAG, "Invalid/stale channel index size (%ld). Deleting and refreshing: %s", file_size, index_path);
        fclose(f);
        unlink(index_path);
        ch->entries = NULL;
        ch->entry_count = 0;
        ch->base.loaded = true;
        if (!ch->refreshing) {
            if (ch->index_io_lock) {
                xSemaphoreGive(ch->index_io_lock);
            }
            return makapix_impl_request_refresh(channel);
        }
        if (ch->index_io_lock) {
            xSemaphoreGive(ch->index_io_lock);
        }
        return ESP_OK;
    }
    
    size_t entry_count = file_size / sizeof(makapix_channel_entry_t);
    
    // Allocate entries array
    ch->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
    if (!ch->entries) {
        fclose(f);
        if (ch->index_io_lock) {
            xSemaphoreGive(ch->index_io_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    
    // Read entries in chunks
    const size_t CHUNK_SIZE = 100;
    size_t read_count = 0;
    while (read_count < entry_count) {
        size_t to_read = (entry_count - read_count > CHUNK_SIZE) ? 
                          CHUNK_SIZE : (entry_count - read_count);
        size_t read = fread(&ch->entries[read_count], 
                            sizeof(makapix_channel_entry_t), to_read, f);
        if (read != to_read) {
            ESP_LOGE(TAG, "Failed to read index entries");
            free(ch->entries);
            ch->entries = NULL;
            fclose(f);
            if (ch->index_io_lock) {
                xSemaphoreGive(ch->index_io_lock);
            }
            return ESP_FAIL;
        }
        read_count += read;
        taskYIELD();
    }
    
    fclose(f);
    
    ch->entry_count = entry_count;
    ch->base.loaded = true;
    
    ESP_LOGD(TAG, "Loaded %zu entries", ch->entry_count);
    
    // Start refresh if not already refreshing
    if (!ch->refreshing) {
        makapix_impl_request_refresh(channel);
    }

    if (ch->index_io_lock) {
        xSemaphoreGive(ch->index_io_lock);
    }
    
    return ESP_OK;
}

static void makapix_impl_unload(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    
    if (ch->navigator_ready) {
        play_navigator_deinit(&ch->navigator);
        ch->navigator_ready = false;
    }

    free(ch->entries);
    ch->entries = NULL;

    ch->entry_count = 0;
    ch->base.loaded = false;
}

static esp_err_t makapix_impl_start_playback(channel_handle_t channel, 
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->base.loaded) return ESP_ERR_INVALID_STATE;

    // Store settings
    ch->base.current_order = order_mode;
    if (filter) {
        ch->base.current_filter = *filter;
    } else {
        memset(&ch->base.current_filter, 0, sizeof(ch->base.current_filter));
    }

    // (Re)initialize playlist-aware navigator
    if (ch->navigator_ready) {
        play_navigator_deinit(&ch->navigator);
        ch->navigator_ready = false;
    }

    play_order_mode_t play_order = PLAY_ORDER_SERVER;
    switch (order_mode) {
        case CHANNEL_ORDER_CREATED: play_order = PLAY_ORDER_CREATED; break;
        case CHANNEL_ORDER_RANDOM:  play_order = PLAY_ORDER_RANDOM;  break;
        case CHANNEL_ORDER_ORIGINAL:
        default:                   play_order = PLAY_ORDER_SERVER;  break;
    }

    uint32_t pe = config_store_get_pe();
    channel_settings_t settings = {0};
    if (channel_settings_load_for_channel_id(ch->channel_id, &settings) != ESP_OK) {
        memset(&settings, 0, sizeof(settings));
    }

    if (settings.pe_present) {
        pe = settings.pe;
    }

    if (settings.channel_dwell_time_present) {
        ch->channel_dwell_override_ms = settings.channel_dwell_time_ms;
    } else {
        ch->channel_dwell_override_ms = 0;
    }

    uint32_t global_seed = config_store_get_global_seed();

    esp_err_t err = play_navigator_init(&ch->navigator, channel, ch->channel_id, play_order, pe, global_seed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init play navigator: %s", esp_err_to_name(err));
        return err;
    }

    play_navigator_set_channel_dwell_override_ms(&ch->navigator, ch->channel_dwell_override_ms);

    // Apply per-channel settings
    play_navigator_set_randomize_playlist(&ch->navigator,
                                         settings.randomize_playlist_present ? settings.randomize_playlist
                                                                             : config_store_get_randomize_playlist());
    play_navigator_set_live_mode(&ch->navigator,
                                 settings.live_mode_present ? settings.live_mode
                                                           : config_store_get_live_mode());

    ch->navigator_ready = true;
    ESP_LOGD(TAG, "Started playback (navigator): posts=%zu order=%d pe=%lu",
             channel_get_post_count(channel), order_mode, (unsigned long)pe);
    
    return ESP_OK;
}

static esp_err_t makapix_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_item || !ch->navigator_ready) return ESP_ERR_NOT_FOUND;
    
    artwork_ref_t art = {0};
    esp_err_t err = play_navigator_next(&ch->navigator, &art);
    if (err != ESP_OK) return err;

    memset(out_item, 0, sizeof(*out_item));
    strlcpy(out_item->filepath, art.filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art.storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;
    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art.dwell_time_ms);
    switch (art.type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }
    
    return ESP_OK;
}

static esp_err_t makapix_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_item || !ch->navigator_ready) return ESP_ERR_NOT_FOUND;
    
    artwork_ref_t art = {0};
    esp_err_t err = play_navigator_prev(&ch->navigator, &art);
    if (err != ESP_OK) return err;

    memset(out_item, 0, sizeof(*out_item));
    strlcpy(out_item->filepath, art.filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art.storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;
    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art.dwell_time_ms);
    switch (art.type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }
    
    return ESP_OK;
}

static esp_err_t makapix_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_item || !ch->navigator_ready) return ESP_ERR_NOT_FOUND;

    artwork_ref_t art = {0};
    esp_err_t err = play_navigator_current(&ch->navigator, &art);
    if (err != ESP_OK) return err;

    memset(out_item, 0, sizeof(*out_item));
    out_item->post_id = art.post_id;
    strlcpy(out_item->filepath, art.filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art.storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;
    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art.dwell_time_ms);
    switch (art.type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }
    return ESP_OK;
}

static esp_err_t makapix_impl_request_reshuffle(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->navigator_ready) return ESP_ERR_INVALID_STATE;
    if (ch->base.current_order != CHANNEL_ORDER_RANDOM) return ESP_OK;
    play_navigator_set_order(&ch->navigator, PLAY_ORDER_RANDOM);
    ESP_LOGD(TAG, "Reshuffled (navigator)");
    return ESP_OK;
}

static esp_err_t makapix_impl_request_refresh(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    if (ch->refreshing) {
        ESP_LOGW(TAG, "Refresh already in progress");
        return ESP_OK;
    }
    
    // Start background refresh task
    ch->refreshing = true;
    BaseType_t ret = xTaskCreate(refresh_task_impl, "makapix_refresh", 24576, ch, 5, &ch->refresh_task);
    if (ret != pdPASS) {
        ch->refreshing = false;
        ESP_LOGE(TAG, "Failed to create refresh task");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGD(TAG, "Refresh task started for channel %s", ch->channel_id);
    return ESP_OK;
}

static esp_err_t makapix_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_stats) return ESP_ERR_INVALID_ARG;
    
    out_stats->total_items = ch->entry_count;
    out_stats->filtered_items = ch->entry_count;
    if (ch->navigator_ready) {
        uint32_t p = 0, q = 0;
        play_navigator_get_position(&ch->navigator, &p, &q);
        (void)q;
        out_stats->current_position = p;
    } else {
        out_stats->current_position = 0;
    }
    
    return ESP_OK;
}

static size_t makapix_impl_get_post_count(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return 0;
    return ch->entry_count;
}

static esp_err_t makapix_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_post) return ESP_ERR_INVALID_ARG;
    if (!ch->base.loaded) return ESP_ERR_INVALID_STATE;
    if (post_index >= ch->entry_count) return ESP_ERR_INVALID_ARG;

    const makapix_channel_entry_t *entry = &ch->entries[post_index];
    memset(out_post, 0, sizeof(*out_post));

    out_post->post_id = entry->post_id;
    out_post->kind = (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) ? CHANNEL_POST_KIND_PLAYLIST : CHANNEL_POST_KIND_ARTWORK;
    out_post->created_at = entry->created_at;
    out_post->metadata_modified_at = (time_t)entry->metadata_modified_at;
    out_post->dwell_time_ms = entry->dwell_time_ms;

    if (out_post->kind == CHANNEL_POST_KIND_PLAYLIST) {
        out_post->u.playlist.total_artworks = entry->total_artworks;
    } else {
        // Fill artwork fields
        build_vault_path(ch, entry, out_post->u.artwork.filepath, sizeof(out_post->u.artwork.filepath));
        bytes_to_uuid(entry->storage_key_uuid, out_post->u.artwork.storage_key, sizeof(out_post->u.artwork.storage_key));
        out_post->u.artwork.art_url[0] = '\0';

        switch (entry->extension) {
            case EXT_WEBP: out_post->u.artwork.type = ASSET_TYPE_WEBP; break;
            case EXT_GIF:  out_post->u.artwork.type = ASSET_TYPE_GIF;  break;
            case EXT_PNG:  out_post->u.artwork.type = ASSET_TYPE_PNG;  break;
            case EXT_JPEG: out_post->u.artwork.type = ASSET_TYPE_JPEG; break;
            default:       out_post->u.artwork.type = ASSET_TYPE_WEBP; break;
        }

        out_post->u.artwork.width = 0;
        out_post->u.artwork.height = 0;
        out_post->u.artwork.frame_count = 0;
        out_post->u.artwork.has_transparency = false;
        out_post->u.artwork.artwork_modified_at = (time_t)entry->artwork_modified_at;
    }

    return ESP_OK;
}

static void makapix_impl_destroy(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    
    // Clear download callback if this channel is the download source
    // This prevents the download task from accessing freed memory
    if (s_download_channel == ch) {
        download_manager_set_next_callback(NULL, NULL);
        s_download_channel = NULL;
        ESP_LOGD(TAG, "Cleared download callback for destroyed channel");
    }
    
    // Stop refresh task if running
    if (ch->refreshing && ch->refresh_task) {
        ch->refreshing = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (ch->refresh_task) {
            vTaskDelete(ch->refresh_task);
        }
        ch->refresh_task = NULL;
    }
    
    makapix_impl_unload(channel);
    if (ch->index_io_lock) {
        vSemaphoreDelete(ch->index_io_lock);
        ch->index_io_lock = NULL;
    }
    free(ch->channel_id);
    free(ch->vault_path);
    free(ch->channels_path);
    free(ch->base.name);
    free(ch);
    
    ESP_LOGD(TAG, "Channel destroyed");
}

static void *makapix_impl_get_navigator(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return NULL;
    return ch->navigator_ready ? (void *)&ch->navigator : NULL;
}

// Public functions

channel_handle_t makapix_channel_create(const char *channel_id, 
                                         const char *name,
                                         const char *vault_path,
                                         const char *channels_path)
{
    if (!channel_id || !vault_path || !channels_path) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    makapix_channel_t *ch = calloc(1, sizeof(makapix_channel_t));
    if (!ch) {
        ESP_LOGE(TAG, "Failed to allocate channel");
        return NULL;
    }
    
    ch->base.ops = &s_makapix_ops;
    ch->base.name = name ? strdup(name) : strdup("Makapix");
    ch->channel_id = strdup(channel_id);
    ch->vault_path = strdup(vault_path);
    ch->channels_path = strdup(channels_path);
    ch->base.current_order = CHANNEL_ORDER_ORIGINAL;

    ch->index_io_lock = xSemaphoreCreateMutex();
    if (!ch->index_io_lock) {
        free(ch->base.name);
        free(ch->channel_id);
        free(ch->vault_path);
        free(ch->channels_path);
        free(ch);
        return NULL;
    }
    
    if (!ch->base.name || !ch->channel_id || !ch->vault_path || !ch->channels_path) {
        vSemaphoreDelete(ch->index_io_lock);
        ch->index_io_lock = NULL;
        free(ch->base.name);
        free(ch->channel_id);
        free(ch->vault_path);
        free(ch->channels_path);
        free(ch);
        return NULL;
    }
    
    ESP_LOGD(TAG, "Created channel: %s (id=%s)", ch->base.name, ch->channel_id);
    return (channel_handle_t)ch;
}

const char *makapix_channel_get_id(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return ch ? ch->channel_id : NULL;
}

bool makapix_channel_is_refreshing(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return ch ? ch->refreshing : false;
}

esp_err_t makapix_channel_count_cached(const char *channel_id,
                                        const char *channels_path,
                                        const char *vault_path,
                                        size_t *out_total,
                                        size_t *out_cached)
{
    if (!channel_id || !channels_path || !vault_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build index path
    char index_path[256];
    snprintf(index_path, sizeof(index_path), "%s/%s.bin", channels_path, channel_id);
    
    // Try to open index file
    FILE *f = fopen(index_path, "rb");
    if (!f) {
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size % sizeof(makapix_channel_entry_t) != 0) {
        fclose(f);
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_OK;
    }
    
    size_t entry_count = (size_t)(file_size / sizeof(makapix_channel_entry_t));
    if (out_total) *out_total = entry_count;
    
    // Count cached artworks
    size_t cached_count = 0;
    makapix_channel_entry_t entry;
    
    for (size_t i = 0; i < entry_count; i++) {
        if (fread(&entry, sizeof(entry), 1, f) != 1) {
            break;
        }
        
        if (entry.kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }
        
        // Build vault path from storage_key_uuid
        char storage_key[37];
        bytes_to_uuid(entry.storage_key_uuid, storage_key, sizeof(storage_key));
        
        uint8_t sha256[32];
        if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
            continue;
        }
        char dir1[3], dir2[3], dir3[3];
        snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
        snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
        snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);
        
        int ext_idx = (entry.extension <= EXT_JPEG) ? entry.extension : EXT_WEBP;
        char file_path[256];
        snprintf(file_path, sizeof(file_path), "%s/%s/%s/%s/%s%s",
                 vault_path, dir1, dir2, dir3, storage_key, s_ext_strings[ext_idx]);
        
        struct stat st;
        if (stat(file_path, &st) == 0) {
            cached_count++;
        }
    }
    
    fclose(f);
    
    if (out_cached) *out_cached = cached_count;
    return ESP_OK;
}

// Struct for sorting entries by created_at
typedef struct {
    uint32_t entry_idx;
    uint32_t created_at;
} download_sort_item_t;

// Comparator for qsort: descending by created_at (newest first)
static int compare_download_items_desc(const void *a, const void *b)
{
    const download_sort_item_t *item_a = (const download_sort_item_t *)a;
    const download_sort_item_t *item_b = (const download_sort_item_t *)b;

    // Descending order: if b > a, return positive (b comes first)
    if (item_b->created_at > item_a->created_at) return 1;
    if (item_b->created_at < item_a->created_at) return -1;
    return 0;
}

// Download callback: called by download manager to get next file
// Downloads are prioritized by post creation date (newest first), regardless of play order
static esp_err_t download_get_next_callback(download_request_t *out_request, void *user_ctx)
{
    (void)user_ctx;
    makapix_channel_t *ch = s_download_channel;

    if (!ch || !out_request) {
        ESP_LOGW(TAG, "download_get_next_callback: invalid args (ch=%p, req=%p)", ch, out_request);
        return ESP_ERR_INVALID_ARG;
    }

    // Safety check: validate critical pointers to prevent use-after-free
    // If the channel is being destroyed, these might be NULL or invalid
    // This is expected during channel switches and is handled gracefully
    if (!ch->entries || !ch->channel_id) {
        ESP_LOGD(TAG, "download_get_next_callback: channel is being destroyed/switched, returning");
        return ESP_ERR_INVALID_STATE;
    }

    if (ch->entry_count == 0) {
        ESP_LOGD(TAG, "download_get_next_callback: channel %s has 0 entries", ch->channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGD(TAG, "download_get_next_callback: scanning %zu entries for channel %s",
             ch->entry_count, ch->channel_id);

    // Count artwork entries
    size_t artwork_count = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            artwork_count++;
        }
    }

    if (artwork_count == 0) {
        ESP_LOGD(TAG, "download_get_next_callback: no artwork entries in channel %s", ch->channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate array for sorting
    download_sort_item_t *items = malloc(artwork_count * sizeof(download_sort_item_t));
    if (!items) {
        ESP_LOGE(TAG, "download_get_next_callback: failed to allocate sort array");
        return ESP_ERR_NO_MEM;
    }

    // Populate with artwork entries
    size_t item_count = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            items[item_count].entry_idx = (uint32_t)i;
            items[item_count].created_at = ch->entries[i].created_at;
            item_count++;
        }
    }

    // Sort by created_at descending (newest first)
    qsort(items, item_count, sizeof(download_sort_item_t), compare_download_items_desc);

    // Find first undownloaded artwork in sorted order
    esp_err_t result = ESP_ERR_NOT_FOUND;
    size_t downloaded_count = 0;

    for (size_t i = 0; i < item_count; i++) {
        uint32_t entry_idx = items[i].entry_idx;
        const makapix_channel_entry_t *entry = &ch->entries[entry_idx];

        // Build vault path
        char vault_path[512];
        build_vault_path(ch, entry, vault_path, sizeof(vault_path));

        // Check if already downloaded
        struct stat st;
        if (stat(vault_path, &st) == 0) {
            downloaded_count++;
            continue;  // Already downloaded, check next
        }

        // Check for .404 marker (file permanently unavailable)
        char marker_path[520];
        snprintf(marker_path, sizeof(marker_path), "%s.404", vault_path);
        if (stat(marker_path, &st) == 0) {
            continue;  // Skip - server returned 404 previously
        }

        // Found a file that needs downloading - fill out the request
        memset(out_request, 0, sizeof(*out_request));
        bytes_to_uuid(entry->storage_key_uuid, out_request->storage_key, sizeof(out_request->storage_key));

        // Build art_url using SHA256 sharding
        uint8_t sha256[32];
        if (storage_key_sha256(out_request->storage_key, sha256) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to compute SHA256 for %s, skipping", out_request->storage_key);
            continue;
        }

        snprintf(out_request->art_url, sizeof(out_request->art_url),
                 "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                 CONFIG_MAKAPIX_CLUB_HOST,
                 (unsigned int)sha256[0], (unsigned int)sha256[1], (unsigned int)sha256[2],
                 out_request->storage_key,
                 s_ext_strings[entry->extension]);

        strlcpy(out_request->filepath, vault_path, sizeof(out_request->filepath));
        strlcpy(out_request->channel_id, ch->channel_id, sizeof(out_request->channel_id));

        ESP_LOGD(TAG, "Next download: %s (created_at=%lu, newest-first priority)",
                 out_request->storage_key, (unsigned long)entry->created_at);

        result = ESP_OK;
        break;
    }

    if (result == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "All files downloaded for channel %s: %zu/%zu artworks cached",
                 ch->channel_id, downloaded_count, item_count);
    }

    free(items);
    return result;
}

esp_err_t makapix_channel_get_next_download(channel_handle_t channel,
                                             download_request_t *out_request)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return download_get_next_callback(out_request, ch);
}

void makapix_channel_setup_download_callback(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) {
        ESP_LOGW(TAG, "Cannot setup download callback: NULL channel");
        return;
    }
    
    s_download_channel = ch;
    download_manager_set_next_callback(download_get_next_callback, NULL);
    ESP_LOGD(TAG, "Download callback setup for channel %s", ch->channel_id);
    
    // Signal that downloads may be available
    download_manager_signal_work_available();
}

