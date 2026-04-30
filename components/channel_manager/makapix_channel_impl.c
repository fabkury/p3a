// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_channel_impl.c
 * @brief Makapix channel interface implementation: creation, queries, lifecycle
 */

#include "makapix_channel_internal.h"
#include "makapix_channel_events.h"
#include "makapix_channel_utils.h"
#include "makapix_api.h"
#include "makapix_artwork.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "channel_settings.h"
#include "channel_cache.h"
#include "psram_alloc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

// NOTE: play_navigator was removed as part of Play Scheduler migration.
// Navigation is now handled by Play Scheduler directly.
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

// Stack size for refresh task (reduced from 24KB to 12KB after analysis)
// This is sufficient for cJSON parsing, file I/O, and MQTT waits with
// heap-allocated response buffers.
#define MAKAPIX_REFRESH_TASK_STACK_SIZE 12288

// Reap the refresh task and bring its static TCB/stack back to a safe-to-reuse
// state. The refresh task signals its parked-sem and self-suspends; this helper
// takes the sem, waits for the task to actually be in eSuspended, deletes it
// externally, then yields long enough for the idle task to drain the
// termination list. Safe no-op when no task exists.
static esp_err_t reap_refresh_task(makapix_channel_t *ch)
{
    if (!ch || !ch->refresh_task) return ESP_OK;

    TaskHandle_t handle = ch->refresh_task;

    // If still doing work, ask the task to wind down cooperatively.
    if (ch->refreshing) {
        ch->refreshing = false;
        makapix_channel_signal_refresh_shutdown();
    }

    // Wait for the task to reach the parked sync point. The task gives this
    // immediately before vTaskSuspend(NULL), after fully exiting its work loop.
    bool parked = false;
    if (ch->refresh_parked_sem) {
        parked = xSemaphoreTake(ch->refresh_parked_sem, pdMS_TO_TICKS(5000)) == pdTRUE;
    }
    makapix_channel_clear_refresh_shutdown();

    if (!parked) {
        // Task is still in its work loop somewhere — likely blocked in a
        // network call that didn't observe the shutdown signal. Force-deleting
        // here would leak its in-flight allocations and possibly leave the
        // channel cache mutex held, so we leave the task in place and bail.
        // The static buffers stay owned by it; the next refresh on this handle
        // will be skipped via the ch->refreshing guard.
        ESP_LOGE(TAG, "Refresh task did not park within 5s; abandoning reap to avoid corrupting in-flight state");
        return ESP_ERR_TIMEOUT;
    }

    // Close the gap between xSemaphoreGive and vTaskSuspend(NULL): wait until
    // the task is observably suspended on its pinned core.
    for (int i = 0; i < 100 && eTaskGetState(handle) != eSuspended; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ch->refresh_task = NULL;
    vTaskDelete(handle);

    // For static allocation, FreeRTOS does not free the TCB itself. After
    // vTaskDelete, the task sits in xTasksWaitingTermination until the pinned
    // core's idle task runs prvCheckTasksWaitingTermination and unlinks it.
    // Until then, the TCB is still in a FreeRTOS internal list and the static
    // buffer must not be reused/freed. (Note: eTaskGetState cannot distinguish
    // "in xTasksWaitingTermination" from "fully unlinked" — both return
    // eDeleted in the SMP variant — so polling that state is not useful here.)
    // Yielding for several ticks gives the idle task on either core ample
    // opportunity to drain the termination list before we reuse the buffer.
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

// Weak symbol for SD pause check
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

// Forward declarations
static esp_err_t makapix_impl_load(channel_handle_t channel);
static void makapix_impl_unload(channel_handle_t channel);
static esp_err_t makapix_impl_start_playback(channel_handle_t channel, 
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter);
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
    .request_reshuffle = makapix_impl_request_reshuffle,
    .request_refresh = makapix_impl_request_refresh,
    .get_stats = makapix_impl_get_stats,
    .destroy = makapix_impl_destroy,
    .get_post_count = makapix_impl_get_post_count,
    .get_post = makapix_impl_get_post,
    .get_navigator = makapix_impl_get_navigator,
};

// Helper: get filter flags from entry
static __attribute__((unused)) channel_filter_flags_t get_entry_flags(const makapix_channel_entry_t *entry)
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

    // Build .cache path (unified persistence format)
    char cache_path[256];
    channel_cache_build_path(ch->channel_id, ch->channels_path, cache_path, sizeof(cache_path));

    // Recover/cleanup cache file before attempting to load
    if (ch->index_io_lock) {
        xSemaphoreTake(ch->index_io_lock, portMAX_DELAY);
    }
    makapix_cache_recover_and_cleanup(cache_path);

    ESP_LOGD(TAG, "Loading channel: %s", ch->base.name);

    // NOTE: Entry data is now managed by channel_cache_t (loaded by Play Scheduler).
    // This function just marks the channel as loaded and starts the refresh task.
    // The refresh task updates the registered cache via channel_cache_merge_posts().

    // Check if cache file exists to determine if we need a refresh
    struct stat st;
    bool need_refresh = (stat(cache_path, &st) != 0);

    if (ch->index_io_lock) {
        xSemaphoreGive(ch->index_io_lock);
    }

    ch->base.loaded = true;

    // Start refresh if not already refreshing
    if (!ch->refreshing) {
        if (need_refresh) {
            ESP_LOGD(TAG, "Starting refresh to populate empty channel");
        }
        esp_err_t refresh_err = makapix_impl_request_refresh(channel);
        if (refresh_err != ESP_OK && need_refresh) {
            // Don't fail the load - return success, Play Scheduler will retry later.
            // This prevents cascade failures when memory is fragmented.
            ESP_LOGW(TAG, "Could not start refresh for empty channel (err=%d), will retry later", refresh_err);
        }
    }

    return ESP_OK;
}

static void makapix_impl_unload(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;

    // NOTE: Entry data is now managed by channel_cache_t, nothing to free here.
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

    // A previous refresh on this handle may have completed and parked itself
    // (refreshing=false, refresh_task still set). Reap it so the static TCB
    // and stack are safe to reuse for the new task.
    if (ch->refresh_task) {
        if (reap_refresh_task(ch) != ESP_OK) {
            ESP_LOGW(TAG, "Could not reap previous refresh task; skipping new refresh");
            return ESP_ERR_INVALID_STATE;
        }
    }

    ch->refreshing = true;

    // Try static stack allocation first (fragmentation-resistant)
    // Pin to Core 0 to avoid interfering with animation rendering on Core 1
    if (ch->refresh_stack && ch->refresh_stack_allocated && ch->refresh_task_buffer) {
        ch->refresh_task = xTaskCreateStaticPinnedToCore(
            refresh_task_impl,
            "makapix_refresh",
            MAKAPIX_REFRESH_TASK_STACK_SIZE,
            ch,
            CONFIG_P3A_NETWORK_TASK_PRIORITY,
            ch->refresh_stack,
            ch->refresh_task_buffer,
            0  // Core 0
        );
        if (ch->refresh_task != NULL) {
            ESP_LOGD(TAG, "Refresh task started (static, Core 0) for channel %s", ch->base.name);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Static task creation failed, trying dynamic allocation");
    }

    // Fallback: try dynamic allocation with progressively smaller stacks
    // This provides graceful degradation if memory is fragmented
    static const size_t stack_sizes[] = {
        MAKAPIX_REFRESH_TASK_STACK_SIZE,  // 12KB - normal
        8192,                              // 8KB - reduced
        6144                               // 6KB - minimum viable
    };

    // Pin to Core 0 to avoid interfering with animation rendering on Core 1
    for (size_t i = 0; i < sizeof(stack_sizes) / sizeof(stack_sizes[0]); i++) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            refresh_task_impl,
            "makapix_refresh",
            stack_sizes[i],
            ch,
            CONFIG_P3A_NETWORK_TASK_PRIORITY,
            &ch->refresh_task,
            0  // Core 0
        );
        if (ret == pdPASS) {
            if (i > 0) {
                ESP_LOGW(TAG, "Refresh task created with reduced stack: %zu bytes (Core 0)", stack_sizes[i]);
            } else {
                ESP_LOGD(TAG, "Refresh task started (dynamic, Core 0) for channel %s", ch->base.name);
            }
            return ESP_OK;
        }
    }

    // All attempts failed
    ch->refreshing = false;
    ESP_LOGE(TAG, "Failed to create refresh task - memory exhausted");
    return ESP_ERR_NO_MEM;
}

static esp_err_t makapix_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_stats) return ESP_ERR_INVALID_ARG;

    // Look up entry count from registered cache
    channel_cache_lifecycle_lock();
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    size_t entry_count = cache ? cache->entry_count : 0;
    channel_cache_lifecycle_unlock();

    out_stats->total_items = entry_count;
    out_stats->filtered_items = entry_count;
    out_stats->current_position = 0;  // Position tracking moved to Play Scheduler

    return ESP_OK;
}

static size_t makapix_impl_get_post_count(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return 0;

    // Look up entry count from registered cache
    channel_cache_lifecycle_lock();
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    size_t count = cache ? cache->entry_count : 0;
    channel_cache_lifecycle_unlock();
    return count;
}

/**
 * @brief Build vault path from entry without needing makapix_channel_t
 * (Local helper for makapix_impl_get_post)
 */
static void build_vault_path_from_entry_impl(const makapix_channel_entry_t *entry,
                                              const char *vault_path,
                                              char *out, size_t out_len)
{
    char storage_key[40];
    bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

    uint8_t sha256[32];
    if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
        snprintf(out, out_len, "%s/%s%s", vault_path, storage_key, s_ext_strings[0]);
        return;
    }

    char dir1[3], dir2[3], dir3[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
    snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);

    int ext_idx = (entry->extension <= 3) ? entry->extension : 0;
    snprintf(out, out_len, "%s/%s/%s/%s/%s%s",
             vault_path, dir1, dir2, dir3, storage_key, s_ext_strings[ext_idx]);
}

static esp_err_t makapix_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_post) return ESP_ERR_INVALID_ARG;
    if (!ch->base.loaded) return ESP_ERR_INVALID_STATE;

    // Look up entry from registered cache. Hold the lifecycle lock for the
    // whole field-extraction so the cache can't be freed mid-copy.
    channel_cache_lifecycle_lock();
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    if (!cache || !cache->entries || post_index >= cache->entry_count) {
        channel_cache_lifecycle_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    const makapix_channel_entry_t *entry = &cache->entries[post_index];
    memset(out_post, 0, sizeof(*out_post));

    out_post->post_id = entry->post_id;
    out_post->kind = (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) ? CHANNEL_POST_KIND_PLAYLIST : CHANNEL_POST_KIND_ARTWORK;
    out_post->created_at = entry->created_at;

    if (out_post->kind == CHANNEL_POST_KIND_PLAYLIST) {
        out_post->u.playlist.total_artworks = entry->total_artworks;
    } else {
        // Fill artwork fields
        build_vault_path_from_entry_impl(entry, ch->vault_path,
                                         out_post->u.artwork.filepath, sizeof(out_post->u.artwork.filepath));
        bytes_to_uuid(entry->storage_key_uuid, out_post->u.artwork.storage_key, sizeof(out_post->u.artwork.storage_key));
        out_post->u.artwork.art_url[0] = '\0';

        switch (entry->extension) {
            case EXT_WEBP: out_post->u.artwork.type = ASSET_TYPE_WEBP; break;
            case EXT_GIF:  out_post->u.artwork.type = ASSET_TYPE_GIF;  break;
            case EXT_PNG:  out_post->u.artwork.type = ASSET_TYPE_PNG;  break;
            case EXT_JPEG: out_post->u.artwork.type = ASSET_TYPE_JPEG; break;
            default:       out_post->u.artwork.type = ASSET_TYPE_WEBP; break;
        }

        out_post->u.artwork.artwork_modified_at = (time_t)entry->artwork_modified_at;
    }
    channel_cache_lifecycle_unlock();

    return ESP_OK;
}

static void makapix_impl_destroy(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;

    // Reap the refresh task before touching its static buffers. If the reap
    // times out we leak the channel handle rather than risk freeing memory
    // that FreeRTOS still holds references to.
    if (ch->refresh_task) {
        if (reap_refresh_task(ch) != ESP_OK) {
            ESP_LOGE(TAG, "Refusing to destroy channel '%s' — refresh task is unresponsive; leaking handle to preserve memory safety",
                     ch->base.name ? ch->base.name : "(unknown)");
            return;
        }
    }

    makapix_impl_unload(channel);
    if (ch->index_io_lock) {
        vSemaphoreDelete(ch->index_io_lock);
        ch->index_io_lock = NULL;
    }
    if (ch->refresh_parked_sem) {
        vSemaphoreDelete(ch->refresh_parked_sem);
        ch->refresh_parked_sem = NULL;
    }

    // Free pre-allocated refresh task resources
    if (ch->refresh_stack_allocated && ch->refresh_stack) {
        free(ch->refresh_stack);
        ch->refresh_stack = NULL;
        ch->refresh_stack_allocated = false;
    }
    if (ch->refresh_task_buffer) {
        heap_caps_free(ch->refresh_task_buffer);
        ch->refresh_task_buffer = NULL;
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
    
    makapix_channel_t *ch = psram_calloc(1, sizeof(makapix_channel_t));
    if (!ch) {
        ESP_LOGE(TAG, "Failed to allocate channel");
        return NULL;
    }

    ch->base.ops = &s_makapix_ops;
    ch->base.name = name ? psram_strdup(name) : psram_strdup("Makapix");
    ch->channel_id = psram_strdup(channel_id);
    ch->vault_path = psram_strdup(vault_path);
    ch->channels_path = psram_strdup(channels_path);
    ch->base.current_order = CHANNEL_ORDER_CREATED;

    ch->index_io_lock = xSemaphoreCreateMutex();
    ch->refresh_parked_sem = xSemaphoreCreateBinary();
    if (!ch->index_io_lock || !ch->refresh_parked_sem) {
        if (ch->index_io_lock) vSemaphoreDelete(ch->index_io_lock);
        if (ch->refresh_parked_sem) vSemaphoreDelete(ch->refresh_parked_sem);
        free(ch->base.name);
        free(ch->channel_id);
        free(ch->vault_path);
        free(ch->channels_path);
        free(ch);
        return NULL;
    }

    if (!ch->base.name || !ch->channel_id || !ch->vault_path || !ch->channels_path) {
        vSemaphoreDelete(ch->index_io_lock);
        vSemaphoreDelete(ch->refresh_parked_sem);
        ch->index_io_lock = NULL;
        ch->refresh_parked_sem = NULL;
        free(ch->base.name);
        free(ch->channel_id);
        free(ch->vault_path);
        free(ch->channels_path);
        free(ch);
        return NULL;
    }

    // Pre-allocate static task resources for refresh task (fragmentation mitigation)
    // This is allocated once at channel creation to avoid heap fragmentation
    // issues after long operation periods (42+ hours).
    // TCB must be in internal RAM (FreeRTOS requirement), stack can be in SPIRAM.
    ch->refresh_task_buffer = heap_caps_malloc(sizeof(StaticTask_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ch->refresh_stack = heap_caps_malloc(MAKAPIX_REFRESH_TASK_STACK_SIZE * sizeof(StackType_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ch->refresh_stack) {
        ch->refresh_stack = heap_caps_malloc(MAKAPIX_REFRESH_TASK_STACK_SIZE * sizeof(StackType_t),
                                              MALLOC_CAP_8BIT);
    }
    ch->refresh_stack_allocated = (ch->refresh_stack != NULL && ch->refresh_task_buffer != NULL);
    if (!ch->refresh_stack_allocated) {
        ESP_LOGW(TAG, "Could not pre-allocate refresh task resources - will use dynamic allocation");
        // Clean up partial allocation
        if (ch->refresh_stack) { free(ch->refresh_stack); ch->refresh_stack = NULL; }
        if (ch->refresh_task_buffer) { heap_caps_free(ch->refresh_task_buffer); ch->refresh_task_buffer = NULL; }
    }

    ESP_LOGD(TAG, "Created channel: %s (id=%s)", ch->base.name, ch->channel_id);
    return (channel_handle_t)ch;
}

const char *makapix_channel_get_id(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return ch ? ch->channel_id : NULL;
}

void makapix_channel_set_spec(channel_handle_t channel, const char *channel_key, const char *identifier)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    if (channel_key) {
        strlcpy(ch->channel_key, channel_key, sizeof(ch->channel_key));
    }
    if (identifier) {
        strlcpy(ch->identifier, identifier, sizeof(ch->identifier));
    }
}

bool makapix_channel_is_refreshing(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return ch ? ch->refreshing : false;
}

esp_err_t makapix_channel_stop_refresh(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ch->refresh_task) {
        return ESP_OK;  // No task to reap
    }

    ESP_LOGI(TAG, "Stopping refresh for channel: %s", ch->base.name ? ch->base.name : "(unknown)");
    return reap_refresh_task(ch);
}

esp_err_t makapix_channel_count_cached(const char *channel_id,
                                        const char *channels_path,
                                        const char *vault_path,
                                        size_t *out_total,
                                        size_t *out_cached)
{
    if (!channel_id || !channels_path) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)vault_path;  // No longer needed - LAi has the count

    // Try in-memory cache first (fast path)
    channel_cache_lifecycle_lock();
    channel_cache_t *cache = channel_cache_registry_find(channel_id);
    if (cache) {
        if (out_total) *out_total = cache->entry_count;
        if (out_cached) *out_cached = cache->available_count;
        channel_cache_lifecycle_unlock();
        return ESP_OK;
    }
    channel_cache_lifecycle_unlock();

    // Fall back to reading .cache file header
    char cache_path[256];
    channel_cache_build_path(channel_id, channels_path, cache_path, sizeof(cache_path));

    FILE *f = fopen(cache_path, "rb");
    if (!f) {
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_ERR_NOT_FOUND;
    }

    // Read and validate header
    channel_cache_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_OK;
    }

    fclose(f);

    // Validate magic
    if (header.magic != CHANNEL_CACHE_MAGIC) {
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_OK;
    }

    if (out_total) *out_total = header.ci_count;
    if (out_cached) *out_cached = header.lai_count;
    return ESP_OK;
}

