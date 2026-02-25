// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "makapix_channel_internal.h"
#include "makapix_api.h"
#include "makapix.h"  // For makapix_ps_refresh_mark_complete()
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "makapix_channel_events.h"
#include "channel_cache.h"
#include "channel_metadata.h"  // For generic metadata save/load
#include "p3a_state.h"      // For PICO-8 mode check
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "makapix_channel_refresh";

// TEMP DEBUG: Instrument rename() failures for channel index atomic writes
#define MAKAPIX_TEMP_DEBUG_RENAME_FAIL 1
#define MAKAPIX_HAVE_STATVFS 0

#if MAKAPIX_TEMP_DEBUG_RENAME_FAIL
static void makapix_temp_debug_log_rename_failure(const char *src_path, const char *dst_path, int rename_errno)
{
    if (!src_path || !dst_path) return;

    ESP_LOGE(TAG,
             "TEMP DEBUG: rename('%s' -> '%s') failed: errno=%d (%s), task='%s', core=%d, uptime_us=%lld",
             src_path,
             dst_path,
             rename_errno,
             strerror(rename_errno),
             pcTaskGetName(NULL),
             xPortGetCoreID(),
             (long long)esp_timer_get_time());

    // Stat source (temp) file
    struct stat st_src = {0};
    if (stat(src_path, &st_src) == 0) {
        ESP_LOGE(TAG,
                 "TEMP DEBUG: src stat ok: mode=0%o size=%ld mtime=%ld",
                 (unsigned int)st_src.st_mode,
                 (long)st_src.st_size,
                 (long)st_src.st_mtime);
    } else {
        int e = errno;
        ESP_LOGE(TAG, "TEMP DEBUG: src stat failed: errno=%d (%s)", e, strerror(e));
    }

    // Stat destination (final) file
    struct stat st_dst = {0};
    if (stat(dst_path, &st_dst) == 0) {
        ESP_LOGE(TAG,
                 "TEMP DEBUG: dst stat ok (dst exists): mode=0%o size=%ld mtime=%ld",
                 (unsigned int)st_dst.st_mode,
                 (long)st_dst.st_size,
                 (long)st_dst.st_mtime);
    } else {
        int e = errno;
        ESP_LOGE(TAG, "TEMP DEBUG: dst stat failed (dst likely missing): errno=%d (%s)", e, strerror(e));
    }

#if MAKAPIX_HAVE_STATVFS
    struct statvfs vfs = {0};
    if (statvfs(dst_path, &vfs) == 0) {
        unsigned long long bsize = (unsigned long long)vfs.f_frsize ? (unsigned long long)vfs.f_frsize
                                                                    : (unsigned long long)vfs.f_bsize;
        unsigned long long free_bytes = bsize * (unsigned long long)vfs.f_bavail;
        unsigned long long total_bytes = bsize * (unsigned long long)vfs.f_blocks;
        ESP_LOGE(TAG,
                 "TEMP DEBUG: statvfs ok: bsize=%llu blocks=%llu bavail=%llu => free=%llu bytes, total=%llu bytes",
                 bsize,
                 (unsigned long long)vfs.f_blocks,
                 (unsigned long long)vfs.f_bavail,
                 free_bytes,
                 total_bytes);
    } else {
        int e = errno;
        ESP_LOGE(TAG, "TEMP DEBUG: statvfs failed: errno=%d (%s)", e, strerror(e));
    }
#else
    ESP_LOGE(TAG, "TEMP DEBUG: statvfs unavailable in this build; skipping free-space report");
#endif
}
#endif

esp_err_t save_channel_metadata(makapix_channel_t *ch, const char *cursor, time_t refresh_time)
{
    channel_metadata_t meta = {
        .last_refresh = refresh_time,
    };
    if (cursor && cursor[0] != '\0') {
        strncpy(meta.cursor, cursor, sizeof(meta.cursor) - 1);
        meta.cursor[sizeof(meta.cursor) - 1] = '\0';
    }
    return channel_metadata_save(ch->channel_id, ch->channels_path, &meta);
}

esp_err_t load_channel_metadata(makapix_channel_t *ch, char *out_cursor, time_t *out_refresh_time)
{
    channel_metadata_t meta;
    esp_err_t err = channel_metadata_load(ch->channel_id, ch->channels_path, &meta);

    if (out_cursor) {
        if (err == ESP_OK) {
            strlcpy(out_cursor, meta.cursor, 64);
        } else {
            out_cursor[0] = '\0';
        }
    }
    if (out_refresh_time) {
        *out_refresh_time = (err == ESP_OK) ? meta.last_refresh : 0;
    }

    return err;
}

/**
 * @brief Merge a batch of posts from refresh into the channel cache
 *
 * Finds the registered cache for the channel and merges the new posts.
 * This is called for each batch received during channel refresh.
 */
static esp_err_t merge_refresh_batch(makapix_channel_t *ch, const makapix_post_t *posts, size_t count)
{
    if (!ch || !posts || count == 0) return ESP_ERR_INVALID_ARG;

    // Find the registered Play Scheduler cache
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    if (!cache) {
        ESP_LOGW(TAG, "Cache not registered for channel '%s', skipping batch", ch->channel_id);
        return ESP_ERR_NOT_FOUND;
    }

    return channel_cache_merge_posts(cache, posts, count, ch->channels_path, ch->vault_path);
}

void refresh_task_impl(void *pvParameters)
{
    makapix_channel_t *ch = (makapix_channel_t *)pvParameters;
    if (!ch) {
        vTaskDelete(NULL);
        return;
    }

    // Wait for MQTT before first query (also wakes on shutdown signal)
    if (!makapix_channel_wait_for_mqtt_or_shutdown(portMAX_DELAY)) {
        ch->refreshing = false;
        ch->refresh_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Build a user-friendly channel name from channel_id for UI display
    char display_name[64];
    if (strcmp(ch->channel_id, "all") == 0) {
        snprintf(display_name, sizeof(display_name), "All Artworks");
    } else if (strcmp(ch->channel_id, "promoted") == 0) {
        snprintf(display_name, sizeof(display_name), "Promoted");
    } else if (strcmp(ch->channel_id, "user") == 0) {
        snprintf(display_name, sizeof(display_name), "My Channel");
    } else if (strncmp(ch->channel_id, "by_user_", 8) == 0) {
        // Truncate user ID to fit in display buffer
        snprintf(display_name, sizeof(display_name), "User: %.48s", ch->channel_id + 8);
    } else if (strncmp(ch->channel_id, "hashtag_", 8) == 0) {
        // Truncate hashtag to fit in display buffer
        snprintf(display_name, sizeof(display_name), "#%.56s", ch->channel_id + 8);
    } else {
        // Truncate channel_id to fit in display buffer
        snprintf(display_name, sizeof(display_name), "Channel: %.52s", ch->channel_id);
    }
    
    // Update UI message to indicate we're updating the index
    // This is called during boot when no animation is loaded yet
    extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
    extern bool animation_player_is_animation_ready(void);
    if (!animation_player_is_animation_ready()) {
        // Still in boot loading phase - update the message with channel name
        p3a_render_set_channel_message(display_name, 1 /* P3A_CHANNEL_MSG_LOADING */, -1, "Updating channel index...");
    }
    
    bool first_query_completed = false;  // Track if we've completed the first query
    
    // Determine channel type from channel_id
    makapix_channel_type_t channel_type = MAKAPIX_CHANNEL_ALL;
    makapix_query_request_t query_req = {0};
    
    if (strcmp(ch->channel_id, "all") == 0) {
        channel_type = MAKAPIX_CHANNEL_ALL;
    } else if (strcmp(ch->channel_id, "promoted") == 0) {
        channel_type = MAKAPIX_CHANNEL_PROMOTED;
    } else if (strcmp(ch->channel_id, "user") == 0) {
        channel_type = MAKAPIX_CHANNEL_USER;
    } else if (strncmp(ch->channel_id, "by_user_", 8) == 0) {
        channel_type = MAKAPIX_CHANNEL_BY_USER;
        strncpy(query_req.user_sqid, ch->channel_id + 8, sizeof(query_req.user_sqid) - 1);
    } else if (strncmp(ch->channel_id, "hashtag_", 8) == 0) {
        channel_type = MAKAPIX_CHANNEL_HASHTAG;
        strncpy(query_req.hashtag, ch->channel_id + 8, sizeof(query_req.hashtag) - 1);
    } else if (strncmp(ch->channel_id, "artwork_", 8) == 0) {
        // Single artwork channel - handled separately
        ch->refreshing = false;
        ch->refresh_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    query_req.channel = channel_type;
    query_req.sort = MAKAPIX_SORT_SERVER_ORDER;
    query_req.limit = 32;
    query_req.has_cursor = false;

    const size_t TARGET_COUNT = config_store_get_channel_cache_size();
    
    // Get refresh interval from NVS (defaults to 3600 = 1 hour)
    uint32_t refresh_interval_sec = config_store_get_refresh_interval_sec();
    
    while (ch->refreshing) {
        // Check for PICO-8 mode - exit refresh cycle cleanly
        if (p3a_state_get() == P3A_STATE_PICO8_STREAMING) {
            ESP_LOGI(TAG, "PICO-8 mode active, exiting refresh cycle");
            break;
        }

        size_t total_queried = 0;
        query_req.has_cursor = false;
        query_req.cursor[0] = '\0';

        // Load saved cursor if exists
        char saved_cursor[64] = {0};
        time_t last_refresh_time = 0;
        load_channel_metadata(ch, saved_cursor, &last_refresh_time);
        if (strlen(saved_cursor) > 0) {
            query_req.has_cursor = true;
            size_t copy_len = strlen(saved_cursor);
            if (copy_len >= sizeof(query_req.cursor)) copy_len = sizeof(query_req.cursor) - 1;
            memcpy(query_req.cursor, saved_cursor, copy_len);
            query_req.cursor[copy_len] = '\0';
        }
        
        // Allocate response on heap
        makapix_query_response_t *resp = malloc(sizeof(makapix_query_response_t));
        if (!resp) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            break;
        }

        // Query posts until we have TARGET_COUNT or no more available
        while (total_queried < TARGET_COUNT && ch->refreshing) {
            memset(resp, 0, sizeof(makapix_query_response_t));
            esp_err_t err = makapix_api_query_posts(&query_req, resp);

            // Check for shutdown or PICO-8 mode after network call
            if (!ch->refreshing) {
                ESP_LOGI(TAG, "Shutdown requested during query, exiting");
                break;
            }
            if (p3a_state_get() == P3A_STATE_PICO8_STREAMING) {
                ESP_LOGI(TAG, "PICO-8 mode activated during query, exiting");
                break;
            }

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Query transport error: %s (0x%x) - check MQTT connection and server availability",
                         esp_err_to_name(err), err);
                break;
            }
            if (!resp->success) {
                ESP_LOGW(TAG, "Query server error: %s (code: %s)",
                         resp->error[0] ? resp->error : "(no message)",
                         resp->error_code[0] ? resp->error_code : "(none)");
                break;
            }

            if (resp->post_count == 0) {
                ESP_LOGD(TAG, "No more posts available");
                break;
            }

            // Merge batch into channel cache
            esp_err_t merge_err = merge_refresh_batch(ch, resp->posts, resp->post_count);
            if (merge_err == ESP_OK) {
                channel_cache_t *diag_cache = channel_cache_registry_find(ch->channel_id);
                ESP_LOGI(TAG, "Batch merged: ch='%s' entry_count=%zu available=%zu",
                         ch->channel_id,
                         diag_cache ? diag_cache->entry_count : 0,
                         diag_cache ? diag_cache->available_count : 0);
            } else {
                ESP_LOGW(TAG, "Batch merge failed: ch='%s' err=%s",
                         ch->channel_id, esp_err_to_name(merge_err));
            }

            // Check for shutdown after index update
            if (!ch->refreshing) {
                ESP_LOGI(TAG, "Shutdown requested during index update, exiting");
                break;
            }

            // Signal download manager to rescan - new index entries arrived
            download_manager_rescan();

            // Free any heap allocations inside parsed posts
            for (size_t pi = 0; pi < resp->post_count; pi++) {
                if (resp->posts[pi].kind == MAKAPIX_POST_KIND_PLAYLIST && resp->posts[pi].artworks) {
                    free(resp->posts[pi].artworks);
                    resp->posts[pi].artworks = NULL;
                    resp->posts[pi].artworks_count = 0;
                }
            }
            total_queried += resp->post_count;
            
            // Save cursor for next query
            if (resp->has_more && strlen(resp->next_cursor) > 0) {
                query_req.has_cursor = true;
                size_t copy_len = strlen(resp->next_cursor);
                if (copy_len >= sizeof(query_req.cursor)) copy_len = sizeof(query_req.cursor) - 1;
                memcpy(query_req.cursor, resp->next_cursor, copy_len);
                query_req.cursor[copy_len] = '\0';
                ESP_LOGI(TAG, "Cursor advanced: len=%zu, preview=%.20s...", copy_len, query_req.cursor);
            } else {
                query_req.has_cursor = false;
                if (resp->has_more) {
                    ESP_LOGW(TAG, "has_more=true but next_cursor is empty! Pagination may be broken. "
                             "total_queried=%zu, entry_count=%zu",
                             total_queried,
                             channel_cache_registry_find(ch->channel_id)
                                 ? channel_cache_registry_find(ch->channel_id)->entry_count : 0);
                }
            }
            
            if (!resp->has_more) {
                break;
            }
            
            if (!first_query_completed && total_queried > 0) {
                first_query_completed = true;
            }
            
            // Delay between queries
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        free(resp);

        // Save metadata
        time_t now = time(NULL);
        save_channel_metadata(ch, query_req.has_cursor ? query_req.cursor : "", now);
        ch->last_refresh_time = now;

        // Check for shutdown after metadata save
        if (!ch->refreshing) break;

        // Count how many are artworks vs playlists using cache lookup
        {
            channel_cache_t *stats_cache = channel_cache_registry_find(ch->channel_id);
            size_t entry_count = stats_cache ? stats_cache->entry_count : 0;
            size_t artwork_count = 0;
            if (stats_cache && stats_cache->entries) {
                for (size_t i = 0; i < stats_cache->entry_count; i++) {
                    if (stats_cache->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
                        artwork_count++;
                    }
                }
            }
            ESP_LOGD(TAG, "Channel %s: %zu entries (%zu artworks)", ch->channel_id, entry_count, artwork_count);
        }

        // Signal that refresh has completed - this unblocks download manager
        makapix_channel_signal_refresh_done();

        // Signal Play Scheduler that this channel's refresh is complete
        makapix_ps_refresh_mark_complete(ch->channel_id);

        // Wait for next refresh cycle - can be woken by REFRESH_IMMEDIATE signal
        uint32_t elapsed = 0;
        while (elapsed < refresh_interval_sec && ch->refreshing) {
            // Check for immediate refresh request (e.g., after PICO-8 exit)
            if (makapix_channel_check_and_clear_refresh_immediate()) {
                ESP_LOGI(TAG, "Immediate refresh requested, starting now");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            elapsed++;
        }
        
        if (!ch->refreshing) {
            break;
        }
        
        // If MQTT disconnected, wait for reconnection (also wakes on shutdown signal)
        if (!makapix_channel_is_mqtt_ready()) {
            if (!makapix_channel_wait_for_mqtt_or_shutdown(portMAX_DELAY)) {
                break;
            }
        }
    }
    
    ch->refreshing = false;
    ch->refresh_task = NULL;
    vTaskDelete(NULL);
}

