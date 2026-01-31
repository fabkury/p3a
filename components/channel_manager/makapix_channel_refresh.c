// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "makapix_channel_internal.h"
#include "makapix_api.h"
#include "makapix_artwork.h"
#include "makapix.h"  // For makapix_ps_refresh_mark_complete()
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "makapix_channel_events.h"
#include "channel_cache.h"  // For LAi synchronous cleanup on eviction
#include "p3a_state.h"      // For PICO-8 mode check
#include "esp_log.h"
#include "esp_timer.h"

// NOTE: play_navigator was removed as part of Play Scheduler migration.
// Live Mode synchronization is now deferred.
// See play_scheduler.c for Live Mode deferred feature notes.
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

// Helper: comparison function for qsort (entries by created_at, oldest first)
static int compare_entries_by_created(const void *a, const void *b)
{
    const makapix_channel_entry_t *ea = (const makapix_channel_entry_t *)a;
    const makapix_channel_entry_t *eb = (const makapix_channel_entry_t *)b;
    if (ea->created_at < eb->created_at) return -1;
    if (ea->created_at > eb->created_at) return 1;
    return 0;
}

/**
 * @brief Remove an entry from LAi when evicting a file
 *
 * Synchronously updates the LAi (Locally Available Index) when a file is
 * deleted due to eviction. This ensures LAi stays consistent with the
 * actual files on disk.
 *
 * @param channel_id Channel identifier (e.g., "all", "promoted")
 * @param post_id Post ID of the evicted entry
 */
static void lai_cleanup_on_eviction(const char *channel_id, int32_t post_id)
{
    if (!channel_id) return;

    channel_cache_t *cache = channel_cache_registry_find(channel_id);
    if (!cache) {
        // Channel cache not loaded/registered - this is normal during early boot
        // or when channel is not currently active in play scheduler
        ESP_LOGD(TAG, "LAi cleanup: cache not found for channel '%s' (post_id=%ld)",
                 channel_id, (long)post_id);
        return;
    }

    if (lai_remove_entry(cache, post_id)) {
        ESP_LOGD(TAG, "LAi cleanup: removed post_id=%ld from channel '%s'",
                 (long)post_id, channel_id);
        // Schedule debounced save to persist LAi changes
        channel_cache_schedule_save(cache);
    }
}

esp_err_t save_channel_metadata(makapix_channel_t *ch, const char *cursor, time_t refresh_time)
{
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", ch->channels_path, ch->channel_id);
    
    cJSON *meta = cJSON_CreateObject();
    if (!meta) return ESP_ERR_NO_MEM;
    
    if (cursor && strlen(cursor) > 0) {
        cJSON_AddStringToObject(meta, "cursor", cursor);
    } else {
        cJSON_AddNullToObject(meta, "cursor");
    }
    cJSON_AddNumberToObject(meta, "last_refresh", (double)refresh_time);
    
    char *json_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!json_str) return ESP_ERR_NO_MEM;
    
    // Atomic write: write to temp file, then rename
    char temp_path[260];
    size_t path_len = strlen(meta_path);
    if (path_len + 4 < sizeof(temp_path)) {
        snprintf(temp_path, sizeof(temp_path), "%s.tmp", meta_path);
    } else {
        ESP_LOGE(TAG, "Meta path too long for temp file");
        free(json_str);
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *f = fopen(temp_path, "w");
    if (!f) {
        free(json_str);
        return ESP_FAIL;
    }
    
    fprintf(f, "%s", json_str);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(json_str);
    
    // Rename temp to final
    if (rename(temp_path, meta_path) != 0) {
        unlink(temp_path);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t load_channel_metadata(makapix_channel_t *ch, char *out_cursor, time_t *out_refresh_time)
{
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", ch->channels_path, ch->channel_id);
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", meta_path);
    struct stat tmp_st;
    if (stat(tmp_path, &tmp_st) == 0 && S_ISREG(tmp_st.st_mode)) {
        ESP_LOGD(TAG, "Removing orphan temp file: %s", tmp_path);
        unlink(tmp_path);
    }
    
    FILE *f = fopen(meta_path, "r");
    if (!f) {
        if (out_cursor) out_cursor[0] = '\0';
        if (out_refresh_time) *out_refresh_time = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 4096) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    char *json_buf = malloc(size + 1);
    if (!json_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    fread(json_buf, 1, size, f);
    json_buf[size] = '\0';
    fclose(f);
    
    cJSON *meta = cJSON_Parse(json_buf);
    free(json_buf);
    if (!meta) return ESP_ERR_INVALID_RESPONSE;
    
    cJSON *cursor = cJSON_GetObjectItem(meta, "cursor");
    if (out_cursor) {
        if (cJSON_IsString(cursor)) {
            strncpy(out_cursor, cursor->valuestring, 63);
            out_cursor[63] = '\0';
        } else {
            out_cursor[0] = '\0';
        }
    }
    
    cJSON *refresh = cJSON_GetObjectItem(meta, "last_refresh");
    if (out_refresh_time) {
        *out_refresh_time = cJSON_IsNumber(refresh) ? (time_t)cJSON_GetNumberValue(refresh) : 0;
    }
    
    cJSON_Delete(meta);
    return ESP_OK;
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

/**
 * @brief Get available storage space on SD card
 * 
 * @param path Path to check (typically /sdcard)
 * @param out_free_bytes Pointer to receive free bytes available
 * @return ESP_OK on success
 */
static esp_err_t get_storage_free_space(const char *path, uint64_t *out_free_bytes)
{
    if (!path || !out_free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Try to get filesystem statistics
    // NOTE: ESP-IDF VFS may not support statvfs on all filesystems
    // For FATFS, we can use a workaround with stat()
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_FAIL;
    }
    
    // Since statvfs is not reliably available on ESP32 FATFS,
    // we use a conservative heuristic: assume storage is "low" 
    // if we can't create a test file. This is a simplified approach.
    // A more robust solution would require FATFS-specific APIs.
    
    // For now, we'll return a "success" with a large value to indicate
    // we can't reliably detect storage pressure via statvfs.
    // The count-based eviction will handle the primary case.
    *out_free_bytes = UINT64_MAX;  // Unknown/unlimited
    
    ESP_LOGD(TAG, "Storage free space check: not fully implemented (using count-based eviction only)");
    return ESP_OK;
}

/**
 * @brief Build vault path from entry without needing makapix_channel_t
 * (Local helper - duplicates logic from channel_cache.c for encapsulation)
 */
static void build_vault_path_from_entry_local(const makapix_channel_entry_t *entry,
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

/**
 * @brief Evict artworks when storage is critically low
 *
 * Per spec: Check available storage and evict oldest files until minimum reserve is met.
 * Only evict from current channel (future: may consider cross-channel eviction).
 * Uses channel_cache for entries lookup.
 *
 * @param ch Channel handle
 * @param min_reserve_bytes Minimum free space to maintain (e.g., 10MB)
 * @return ESP_OK on success
 */
static esp_err_t evict_for_storage_pressure(makapix_channel_t *ch, size_t min_reserve_bytes)
{
    if (!ch) return ESP_ERR_INVALID_ARG;

    uint64_t free_bytes = 0;
    esp_err_t err = get_storage_free_space("/sdcard", &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not determine free storage space");
        return ESP_OK;  // Skip storage-based eviction if we can't measure it
    }

    // If we have enough free space, no action needed
    if (free_bytes == UINT64_MAX || free_bytes >= (uint64_t)min_reserve_bytes) {
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Storage pressure detected: %llu bytes free, %zu bytes required",
             (unsigned long long)free_bytes, min_reserve_bytes);

    // Look up channel cache
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    if (!cache) {
        ESP_LOGW(TAG, "No cache to evict for storage pressure");
        return ESP_OK;
    }

    // Take mutex to safely read cache fields (entries, lai_hash, available_count)
    // This prevents race conditions with download manager's lai_add_entry
    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    if (!cache->entries || cache->entry_count == 0) {
        xSemaphoreGive(cache->mutex);
        ESP_LOGW(TAG, "No entries to evict for storage pressure");
        return ESP_OK;
    }

    // Use LAi to determine downloaded count (O(1) vs filesystem I/O)
    size_t downloaded_count = cache->available_count;

    if (downloaded_count == 0) {
        xSemaphoreGive(cache->mutex);
        ESP_LOGW(TAG, "No files to evict for storage pressure");
        return ESP_OK;
    }

    makapix_channel_entry_t *downloaded = malloc(downloaded_count * sizeof(makapix_channel_entry_t));
    if (!downloaded) {
        xSemaphoreGive(cache->mutex);
        return ESP_ERR_NO_MEM;
    }

    // Collect entries that are in LAi (downloaded) - must hold mutex while accessing lai_hash
    size_t di = 0;
    for (size_t i = 0; i < cache->entry_count && di < downloaded_count; i++) {
        if (cache->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            // Check if this post_id is in LAi using the hash table (O(1) lookup)
            int32_t post_id = cache->entries[i].post_id;
            lai_post_id_node_t *node;
            HASH_FIND_INT(cache->lai_hash, &post_id, node);
            if (node) {
                downloaded[di++] = cache->entries[i];
            }
        }
    }

    // Release mutex before file I/O operations (lai_cleanup_on_eviction takes its own mutex)
    xSemaphoreGive(cache->mutex);

    // Update downloaded_count to actual count collected (may differ if entries weren't all artworks)
    downloaded_count = di;

    if (downloaded_count == 0) {
        free(downloaded);
        ESP_LOGW(TAG, "No downloaded artwork files to evict for storage pressure");
        return ESP_OK;
    }

    // Sort by created_at (oldest first)
    qsort(downloaded, downloaded_count, sizeof(makapix_channel_entry_t), compare_entries_by_created);

    // Evict oldest files in batches until we have enough space
    const size_t EVICTION_BATCH = 16;  // Smaller batches for storage pressure
    size_t actually_deleted = 0;

    for (size_t batch_start = 0; batch_start < downloaded_count; batch_start += EVICTION_BATCH) {
        size_t batch_end = batch_start + EVICTION_BATCH;
        if (batch_end > downloaded_count) {
            batch_end = downloaded_count;
        }

        // Delete this batch and synchronously update LAi
        for (size_t i = batch_start; i < batch_end; i++) {
            char file_path[512];
            build_vault_path_from_entry_local(&downloaded[i], ch->vault_path, file_path, sizeof(file_path));
            if (unlink(file_path) == 0) {
                actually_deleted++;
                // Synchronously update LAi to remove the evicted entry
                lai_cleanup_on_eviction(ch->channel_id, downloaded[i].post_id);
            }
        }

        // Re-check free space
        err = get_storage_free_space("/sdcard", &free_bytes);
        if (err == ESP_OK && (free_bytes == UINT64_MAX || free_bytes >= (uint64_t)min_reserve_bytes)) {
            ESP_LOGD(TAG, "Storage pressure relieved after evicting %zu files", actually_deleted);
            break;
        }
    }

    free(downloaded);

    ESP_LOGD(TAG, "Storage-based eviction: deleted %zu files", actually_deleted);
    return ESP_OK;
}

esp_err_t evict_excess_artworks(makapix_channel_t *ch, size_t max_count)
{
    if (!ch) return ESP_ERR_INVALID_ARG;

    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    if (!cache) return ESP_OK;

    size_t evicted = channel_cache_evict_excess(cache, max_count, ch->vault_path);
    if (evicted > 0) {
        ESP_LOGI(TAG, "Evicted %zu artwork files (limit: %zu)", evicted, max_count);
    }
    return ESP_OK;
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
    query_req.pe_present = true;
    query_req.pe = (uint16_t)config_store_get_pe();
    
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

            // Per-batch eviction: if Ci exceeds limit after adding this batch,
            // evict oldest files immediately to maintain invariant.
            // This ensures Ci transitions from one valid state to another.
            {
                channel_cache_t *batch_cache = channel_cache_registry_find(ch->channel_id);
                size_t entry_count = batch_cache ? batch_cache->entry_count : 0;
                if (entry_count > TARGET_COUNT) {
                    ESP_LOGD(TAG, "Per-batch eviction: entry_count=%zu exceeds limit=%zu",
                             entry_count, TARGET_COUNT);
                    evict_excess_artworks(ch, TARGET_COUNT);
                }
            }

            // Check for shutdown after index update
            if (!ch->refreshing) {
                ESP_LOGI(TAG, "Shutdown requested during index update, exiting");
                break;
            }

            // Signal download manager to rescan - new index entries arrived
            download_manager_rescan();

            // NOTE: Live Mode schedule marking was here but has been removed.
            // Live Mode is now deferred. See play_scheduler.c for notes.

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
            } else {
                query_req.has_cursor = false;
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

        // Storage-aware eviction: ensure minimum free space (10MB reserve)
        // NOTE: Only evicts from current channel per spec. Future consideration:
        // cross-channel eviction would require loading all channel indices simultaneously,
        // which may not be feasible with memory constraints.
        const size_t MIN_RESERVE_BYTES = 10 * 1024 * 1024;  // 10MB minimum free space
        evict_for_storage_pressure(ch, MIN_RESERVE_BYTES);

        // Check for shutdown after storage eviction
        if (!ch->refreshing) {
            ESP_LOGI(TAG, "Shutdown requested during eviction, exiting");
            break;
        }

        // Count-based eviction: ensure we don't exceed 1,024 artworks per channel
        evict_excess_artworks(ch, TARGET_COUNT);

        // Check for shutdown after count eviction
        if (!ch->refreshing) break;

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

