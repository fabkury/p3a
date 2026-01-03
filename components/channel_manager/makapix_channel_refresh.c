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
 * @brief Reconcile local index with server data by detecting and removing deleted artworks
 * 
 * Full reconciliation approach (Option B): Compare local index against server response.
 * Any local entries not present in server data are considered deleted.
 * 
 * @param ch Channel handle
 * @param server_post_ids Array of post_ids from server
 * @param server_count Number of post_ids in array
 * @return ESP_OK on success
 */
static esp_err_t reconcile_deletions(makapix_channel_t *ch, 
                                      const int32_t *server_post_ids, 
                                      size_t server_count)
{
    if (!ch || !server_post_ids || server_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ch->entry_count == 0) {
        return ESP_OK;  // Nothing to reconcile
    }
    
    size_t deleted_count = 0;
    size_t kept_count = 0;
    makapix_channel_entry_t *kept_entries = malloc(ch->entry_count * sizeof(makapix_channel_entry_t));
    if (!kept_entries) {
        ESP_LOGE(TAG, "Failed to allocate memory for reconciliation");
        return ESP_ERR_NO_MEM;
    }
    
    // Scan local entries and check if they exist in server data
    for (size_t i = 0; i < ch->entry_count; i++) {
        const makapix_channel_entry_t *entry = &ch->entries[i];
        bool found_on_server = false;
        
        // Linear search in server_post_ids (acceptable for small batches)
        for (size_t j = 0; j < server_count; j++) {
            if (entry->post_id == server_post_ids[j]) {
                found_on_server = true;
                break;
            }
        }
        
        if (found_on_server) {
            // Keep this entry
            kept_entries[kept_count++] = *entry;
        } else {
            // Entry deleted on server - remove local file if it exists
            deleted_count++;
            
            if (entry->kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
                char vault_path[512];
                build_vault_path(ch, entry, vault_path, sizeof(vault_path));
                
                struct stat st;
                if (stat(vault_path, &st) == 0) {
                    if (unlink(vault_path) == 0) {
                        ESP_LOGD(TAG, "Deleted local file for removed artwork: post_id=%ld", 
                                (long)entry->post_id);
                    } else {
                        ESP_LOGW(TAG, "Failed to delete file for removed artwork: post_id=%ld, path=%s", 
                                (long)entry->post_id, vault_path);
                    }
                }
            }
            
            ESP_LOGD(TAG, "Reconciliation: removed post_id=%ld (not on server)", 
                    (long)entry->post_id);
        }
    }
    
    // Update channel entries with kept entries only
    if (deleted_count > 0) {
        free(ch->entries);
        ch->entries = kept_entries;
        ch->entry_count = kept_count;
        ESP_LOGD(TAG, "Reconciled: %zu deleted", deleted_count);
    } else {
        free(kept_entries);
    }
    
    return ESP_OK;
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

esp_err_t update_index_bin(makapix_channel_t *ch, const makapix_post_t *posts, size_t count)
{
    if (!ch || !posts) return ESP_ERR_INVALID_ARG;

    char index_path[256];
    build_index_path(ch, index_path, sizeof(index_path));

    const bool have_lock = (ch->index_io_lock != NULL);
    if (have_lock) {
        xSemaphoreTake(ch->index_io_lock, portMAX_DELAY);
    }
    esp_err_t ret = ESP_OK;

    // Ensure directory exists - create recursively
    char dir_path[256];
    strncpy(dir_path, index_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    char *dir_sep = strrchr(dir_path, '/');
    if (dir_sep) {
        *dir_sep = '\0';
        for (char *p = dir_path + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                struct stat st;
                if (stat(dir_path, &st) != 0) {
                    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                        ESP_LOGE(TAG, "Failed to create directory %s: %d", dir_path, errno);
                    }
                }
                *p = '/';
            }
        }
        struct stat st;
        if (stat(dir_path, &st) != 0) {
            if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "Failed to create directory %s: %d", dir_path, errno);
            }
        }
    }

    // Copy existing entries
    makapix_channel_entry_t *all_entries = NULL;
    size_t all_count = ch->entry_count;
    if (ch->entries && ch->entry_count > 0) {
        all_entries = malloc((all_count + count) * sizeof(makapix_channel_entry_t));
        if (!all_entries) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
        memcpy(all_entries, ch->entries, all_count * sizeof(makapix_channel_entry_t));
    } else {
        all_entries = malloc(count * sizeof(makapix_channel_entry_t));
        if (!all_entries) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
        all_count = 0;
    }

    for (size_t i = 0; i < count; i++) {
        const makapix_post_t *post = &posts[i];

        // Find existing entry by (post_id, kind)
        int found_idx = -1;
        for (size_t j = 0; j < all_count; j++) {
            if (all_entries[j].post_id == post->post_id && all_entries[j].kind == (uint8_t)post->kind) {
                found_idx = (int)j;
                break;
            }
        }

        makapix_channel_entry_t tmp = {0};
        tmp.post_id = post->post_id;
        tmp.kind = (uint8_t)post->kind;
        tmp.created_at = (uint32_t)parse_iso8601_utc(post->created_at);
        tmp.metadata_modified_at = (uint32_t)parse_iso8601_utc(post->metadata_modified_at);
        tmp.filter_flags = 0;

        if (post->kind == MAKAPIX_POST_KIND_ARTWORK) {
            uint8_t uuid_bytes[16] = {0};
            if (!uuid_to_bytes(post->storage_key, uuid_bytes)) {
                ESP_LOGW(TAG, "Failed to parse storage_key UUID: %s", post->storage_key);
                continue;
            }
            memcpy(tmp.storage_key_uuid, uuid_bytes, sizeof(tmp.storage_key_uuid));
            tmp.extension = detect_file_type(post->art_url);
            tmp.artwork_modified_at = (uint32_t)parse_iso8601_utc(post->artwork_modified_at);
            tmp.dwell_time_ms = post->dwell_time_ms;
            tmp.total_artworks = 0;
        } else if (post->kind == MAKAPIX_POST_KIND_PLAYLIST) {
            tmp.extension = 0;
            tmp.artwork_modified_at = 0;
            tmp.dwell_time_ms = post->playlist_dwell_time_ms;
            tmp.total_artworks = post->total_artworks;
            memset(tmp.storage_key_uuid, 0, sizeof(tmp.storage_key_uuid));

            // Best-effort: write/update playlist cache on disk
            playlist_metadata_t playlist = {0};
            playlist.post_id = post->post_id;
            playlist.total_artworks = post->total_artworks;
            playlist.loaded_artworks = 0;
            playlist.available_artworks = 0;
            playlist.dwell_time_ms = post->playlist_dwell_time_ms;
            playlist.metadata_modified_at = parse_iso8601_utc(post->metadata_modified_at);

            if (post->artworks_count > 0 && post->artworks) {
                playlist.artworks = calloc(post->artworks_count, sizeof(artwork_ref_t));
                if (playlist.artworks) {
                    playlist.loaded_artworks = (int32_t)post->artworks_count;
                    for (size_t ai = 0; ai < post->artworks_count; ai++) {
                        const makapix_artwork_t *src = &post->artworks[ai];
                        artwork_ref_t *dst = &playlist.artworks[ai];
                        memset(dst, 0, sizeof(*dst));
                        dst->post_id = src->post_id;
                        strncpy(dst->storage_key, src->storage_key, sizeof(dst->storage_key) - 1);
                        strlcpy(dst->art_url, src->art_url, sizeof(dst->art_url));
                        dst->dwell_time_ms = src->dwell_time_ms;
                        dst->metadata_modified_at = parse_iso8601_utc(src->metadata_modified_at);
                        dst->artwork_modified_at = parse_iso8601_utc(src->artwork_modified_at);
                        dst->width = (uint16_t)src->width;
                        dst->height = (uint16_t)src->height;
                        dst->frame_count = (uint16_t)src->frame_count;
                        dst->has_transparency = src->has_transparency;

                        // Determine type from URL extension
                        switch (detect_file_type(src->art_url)) {
                            case EXT_WEBP: dst->type = ASSET_TYPE_WEBP; break;
                            case EXT_GIF:  dst->type = ASSET_TYPE_GIF;  break;
                            case EXT_PNG:  dst->type = ASSET_TYPE_PNG;  break;
                            case EXT_JPEG: dst->type = ASSET_TYPE_JPEG; break;
                            default:       dst->type = ASSET_TYPE_WEBP; break;
                        }

                        // Downloaded? (file exists in vault)
                        char vault_file[512];
                        build_vault_path_from_storage_key(ch, src->storage_key, detect_file_type(src->art_url), vault_file, sizeof(vault_file));
                        struct stat st;
                        dst->downloaded = (stat(vault_file, &st) == 0);
                        strlcpy(dst->filepath, vault_file, sizeof(dst->filepath));
                        // Note: Downloads are handled automatically by download_manager's single-download-at-a-time approach
                    }

                    // available_artworks is informational only
                    int32_t cnt = 0;
                    for (int32_t j = 0; j < playlist.loaded_artworks; j++) {
                        if (playlist.artworks[j].downloaded) cnt++;
                    }
                    playlist.available_artworks = cnt;
                }
            }

            // Save and free temporary playlist structure
            if (playlist.artworks) {
                playlist_save_to_disk(&playlist);
                free(playlist.artworks);
                playlist.artworks = NULL;
            } else {
                playlist_save_to_disk(&playlist);
            }
        } else {
            continue;
        }

        if (found_idx >= 0) {
            // Existing entry - check for artwork file updates
            if (post->kind == MAKAPIX_POST_KIND_ARTWORK) {
                uint32_t old_artwork_modified = all_entries[(size_t)found_idx].artwork_modified_at;
                uint32_t new_artwork_modified = tmp.artwork_modified_at;
                
                // If artwork file timestamp changed, delete local file to trigger re-download
                if (old_artwork_modified != 0 && new_artwork_modified != 0 && 
                    old_artwork_modified != new_artwork_modified) {
                    
                    char vault_path[512];
                    build_vault_path(ch, &all_entries[(size_t)found_idx], vault_path, sizeof(vault_path));
                    
                    struct stat st;
                    if (stat(vault_path, &st) == 0) {
                        ESP_LOGD(TAG, "Artwork file updated on server (post_id=%ld), deleting local copy for re-download", 
                                (long)post->post_id);
                        if (unlink(vault_path) != 0) {
                            ESP_LOGW(TAG, "Failed to delete outdated artwork file: %s", vault_path);
                        }
                    }
                }
            }
            
            // Update the entry with new metadata
            all_entries[(size_t)found_idx] = tmp;
        } else {
            all_entries[all_count++] = tmp;
        }
    }

    // Atomic write channel index (.bin)
    char temp_path[260];
    size_t path_len = strlen(index_path);
    if (path_len + 4 >= sizeof(temp_path)) {
        ESP_LOGE(TAG, "Index path too long for temp file");
        ret = ESP_ERR_INVALID_ARG;
        goto out;
    }
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", index_path);

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %d", temp_path, errno);
        ret = ESP_FAIL;
        goto out;
    }
    size_t written = fwrite(all_entries, sizeof(makapix_channel_entry_t), all_count, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (written != all_count) {
        ESP_LOGE(TAG, "Failed to write channel index: %zu/%zu", written, all_count);
        unlink(temp_path);
        ret = ESP_FAIL;
        goto out;
    }

    // FATFS rename() won't overwrite an existing destination: remove old index first
    if (unlink(index_path) != 0) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "Failed to unlink old index before rename: %s (errno=%d)", index_path, errno);
        }
    }

    if (rename(temp_path, index_path) != 0) {
        const int rename_errno = errno;
#if MAKAPIX_TEMP_DEBUG_RENAME_FAIL
        makapix_temp_debug_log_rename_failure(temp_path, index_path, rename_errno);
#endif
        if (rename_errno == EEXIST) {
            (void)unlink(index_path);
            if (rename(temp_path, index_path) == 0) {
                goto rename_ok;
            }
        }

        ESP_LOGE(TAG, "Failed to rename index temp file: %d", rename_errno);
        ret = ESP_FAIL;
        goto out;
    }

rename_ok:
    free(ch->entries);
    ch->entries = all_entries;
    ch->entry_count = all_count;

    ESP_LOGD(TAG, "Index updated: %zu entries", all_count);
    ret = ESP_OK;
    all_entries = NULL; // ownership transferred to ch

out:
    if (ret != ESP_OK) {
        free(all_entries);
    }
    if (have_lock) {
        xSemaphoreGive(ch->index_io_lock);
    }
    return ret;
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
 * @brief Evict artworks when storage is critically low
 * 
 * Per spec: Check available storage and evict oldest files until minimum reserve is met.
 * Only evict from current channel (future: may consider cross-channel eviction).
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
    
    // Collect all downloaded artwork files from this channel
    size_t downloaded_count = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            char vault_path[512];
            build_vault_path(ch, &ch->entries[i], vault_path, sizeof(vault_path));
            struct stat st;
            if (stat(vault_path, &st) == 0) {
                downloaded_count++;
            }
        }
    }
    
    if (downloaded_count == 0) {
        ESP_LOGW(TAG, "No files to evict for storage pressure");
        return ESP_OK;
    }
    
    makapix_channel_entry_t *downloaded = malloc(downloaded_count * sizeof(makapix_channel_entry_t));
    if (!downloaded) return ESP_ERR_NO_MEM;
    
    size_t di = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            char vault_path[512];
            build_vault_path(ch, &ch->entries[i], vault_path, sizeof(vault_path));
            struct stat st;
            if (stat(vault_path, &st) == 0) {
                downloaded[di++] = ch->entries[i];
            }
        }
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
        
        // Delete this batch
        for (size_t i = batch_start; i < batch_end; i++) {
            char vault_path[512];
            build_vault_path(ch, &downloaded[i], vault_path, sizeof(vault_path));
            if (unlink(vault_path) == 0) {
                actually_deleted++;
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

    // Count artwork entries that actually have files downloaded
    size_t downloaded_count = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            char vault_path[512];
            build_vault_path(ch, &ch->entries[i], vault_path, sizeof(vault_path));
            struct stat st;
            if (stat(vault_path, &st) == 0) {
                downloaded_count++;
            }
        }
    }

    if (downloaded_count <= max_count) return ESP_OK;

    ESP_LOGD(TAG, "Eviction needed: %zu downloaded files exceed limit of %zu", 
             downloaded_count, max_count);

    // Collect only artwork entries that have files
    makapix_channel_entry_t *downloaded = malloc(downloaded_count * sizeof(makapix_channel_entry_t));
    if (!downloaded) return ESP_ERR_NO_MEM;

    size_t di = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            char vault_path[512];
            build_vault_path(ch, &ch->entries[i], vault_path, sizeof(vault_path));
            struct stat st;
            if (stat(vault_path, &st) == 0) {
                downloaded[di++] = ch->entries[i];
            }
        }
    }

    // Sort by created_at (oldest first)
    qsort(downloaded, downloaded_count, sizeof(makapix_channel_entry_t), compare_entries_by_created);

    // Evict in batches of 32
    const size_t EVICTION_BATCH = 32;
    size_t excess = downloaded_count - max_count;
    size_t to_delete = ((excess + EVICTION_BATCH - 1) / EVICTION_BATCH) * EVICTION_BATCH;
    if (to_delete > downloaded_count) {
        to_delete = downloaded_count;
    }

    // Delete oldest artwork FILES (but keep their index entries)
    size_t actually_deleted = 0;
    for (size_t i = 0; i < to_delete; i++) {
        char vault_path[512];
        build_vault_path(ch, &downloaded[i], vault_path, sizeof(vault_path));
        if (unlink(vault_path) == 0) {
            actually_deleted++;
        }
    }

    free(downloaded);

    ESP_LOGD(TAG, "Evicted %zu artwork files to stay within limit of %zu", 
             actually_deleted, max_count);
    
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
    
    // Update UI message to indicate we're updating the index
    // This is called during boot when no animation is loaded yet
    extern void p3a_render_set_channel_message(const char *channel_name, int msg_type, int progress_percent, const char *detail);
    extern bool animation_player_is_animation_ready(void);
    if (!animation_player_is_animation_ready()) {
        // Still in boot loading phase - update the message
        p3a_render_set_channel_message("Makapix Club", 1 /* P3A_CHANNEL_MSG_LOADING */, -1, "Updating channel index...");
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
    
    const size_t TARGET_COUNT = 1024;
    
    // Get refresh interval from NVS (defaults to 3600 = 1 hour)
    uint32_t refresh_interval_sec = config_store_get_refresh_interval_sec();
    
    while (ch->refreshing) {
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
        
        // Allocate array to track all server post_ids for reconciliation
        int32_t *all_server_post_ids = malloc(TARGET_COUNT * sizeof(int32_t));
        size_t all_server_count = 0;
        if (!all_server_post_ids) {
            ESP_LOGE(TAG, "Failed to allocate reconciliation buffer");
            free(resp);
            break;
        }
        
        // Query posts until we have TARGET_COUNT or no more available
        while (total_queried < TARGET_COUNT && ch->refreshing) {
            memset(resp, 0, sizeof(makapix_query_response_t));
            esp_err_t err = makapix_api_query_posts(&query_req, resp);

            // Check for shutdown after network call
            if (!ch->refreshing) {
                ESP_LOGI(TAG, "Shutdown requested during query, exiting");
                break;
            }

            if (err != ESP_OK || !resp->success) {
                ESP_LOGW(TAG, "Query failed: %s", resp->error);
                break;
            }

            if (resp->post_count == 0) {
                ESP_LOGD(TAG, "No more posts available");
                break;
            }

            // Collect post_ids for reconciliation
            for (size_t pi = 0; pi < resp->post_count && all_server_count < TARGET_COUNT; pi++) {
                all_server_post_ids[all_server_count++] = resp->posts[pi].post_id;
            }

            // Update channel index with new posts
            update_index_bin(ch, resp->posts, resp->post_count);

            // Check for shutdown after index update
            if (!ch->refreshing) {
                ESP_LOGI(TAG, "Shutdown requested during index update, exiting");
                break;
            }

            // Signal download manager that new files may be available
            download_manager_signal_work_available();

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
        
        // Reconcile deletions: remove local entries not present on server
        if (all_server_count > 0) {
            reconcile_deletions(ch, all_server_post_ids, all_server_count);
        }
        free(all_server_post_ids);

        // Check for shutdown after reconciliation
        if (!ch->refreshing) {
            ESP_LOGI(TAG, "Shutdown requested during reconciliation, exiting");
            break;
        }

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

        // Count how many are artworks vs playlists
        size_t artwork_count = 0;
        size_t playlist_count = 0;
        for (size_t i = 0; i < ch->entry_count; i++) {
            if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
                artwork_count++;
            } else {
                playlist_count++;
            }
        }
        
        ESP_LOGD(TAG, "Channel %s: %zu entries (%zu artworks)", ch->channel_id, ch->entry_count, artwork_count);

        // Signal that refresh has completed - this unblocks download manager
        makapix_channel_signal_refresh_done();

        // Signal Play Scheduler that this channel's refresh is complete
        makapix_ps_refresh_mark_complete(ch->channel_id);

        // Wait for next refresh cycle
        for (uint32_t elapsed = 0; elapsed < refresh_interval_sec && ch->refreshing; elapsed++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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

