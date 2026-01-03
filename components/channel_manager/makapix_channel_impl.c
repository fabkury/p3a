// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "makapix_channel_internal.h"
#include "makapix_channel_events.h"
#include "makapix_api.h"
#include "makapix_artwork.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "channel_settings.h"
#include "esp_log.h"
#include "esp_random.h"

// NOTE: play_navigator was removed as part of Play Scheduler migration.
// Navigation is now handled by Play Scheduler directly.
// The legacy navigation functions below return ESP_ERR_NOT_SUPPORTED.
// See play_scheduler.c for Live Mode deferred feature notes.
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

    free(ch->entries);
    ch->entries = NULL;

    ch->entry_count = 0;
    ch->base.loaded = false;
}

static esp_err_t makapix_impl_start_playback(channel_handle_t channel,
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter)
{
    // DEPRECATED: Navigation is now handled by Play Scheduler directly.
    // This function is kept for interface compatibility but does nothing.
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->base.loaded) return ESP_ERR_INVALID_STATE;

    // Store settings for compatibility
    ch->base.current_order = order_mode;
    if (filter) {
        ch->base.current_filter = *filter;
    } else {
        memset(&ch->base.current_filter, 0, sizeof(ch->base.current_filter));
    }

    // Load channel-specific dwell time if present
    channel_settings_t settings = {0};
    if (channel_settings_load_for_channel_id(ch->channel_id, &settings) == ESP_OK) {
        if (settings.channel_dwell_time_present) {
            ch->channel_dwell_override_ms = settings.channel_dwell_time_ms;
        }
    }

    ESP_LOGD(TAG, "start_playback called but navigation now handled by Play Scheduler");
    return ESP_OK;
}

static esp_err_t makapix_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    // DEPRECATED: Navigation is now handled by Play Scheduler directly.
    (void)channel;
    (void)out_item;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t makapix_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    // DEPRECATED: Navigation is now handled by Play Scheduler directly.
    (void)channel;
    (void)out_item;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t makapix_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    // DEPRECATED: Navigation is now handled by Play Scheduler directly.
    (void)channel;
    (void)out_item;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t makapix_impl_request_reshuffle(channel_handle_t channel)
{
    // DEPRECATED: Navigation is now handled by Play Scheduler directly.
    (void)channel;
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
    out_stats->current_position = 0;  // Position tracking moved to Play Scheduler

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

    // Stop refresh task if running (event-driven shutdown)
    if (ch->refreshing && ch->refresh_task) {
        ESP_LOGI(TAG, "Stopping refresh task...");
        ch->refreshing = false;

        // Signal shutdown to wake any blocked waits
        makapix_channel_signal_refresh_shutdown();

        // Wait for task to exit gracefully (up to 5 seconds)
        const int MAX_WAIT_ITERS = 50;  // 50 x 100ms = 5 seconds
        for (int i = 0; i < MAX_WAIT_ITERS && ch->refresh_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Clear shutdown signal for next channel
        makapix_channel_clear_refresh_shutdown();

        // Only force delete if task didn't exit gracefully
        if (ch->refresh_task) {
            ESP_LOGW(TAG, "Refresh task did not exit gracefully, forcing delete");
            vTaskDelete(ch->refresh_task);
            ch->refresh_task = NULL;
        } else {
            ESP_LOGI(TAG, "Refresh task exited cleanly");
        }
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
    // DEPRECATED: Navigation is now handled by Play Scheduler directly.
    (void)channel;
    return NULL;
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

