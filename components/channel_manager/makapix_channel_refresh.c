#include "makapix_channel_internal.h"
#include "makapix_api.h"
#include "makapix_artwork.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "play_navigator.h"
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

// Helper: comparison function for qsort (entries by created_at, oldest first)
static int compare_entries_by_created(const void *a, const void *b)
{
    const makapix_channel_entry_t *ea = (const makapix_channel_entry_t *)a;
    const makapix_channel_entry_t *eb = (const makapix_channel_entry_t *)b;
    if (ea->created_at < eb->created_at) return -1;
    if (ea->created_at > eb->created_at) return 1;
    return 0;
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

                        // Opportunistic background download of the first PE items
                        uint32_t pe_setting = config_store_get_pe();
                        uint32_t want = (pe_setting == 0) ? 32 : pe_setting;
                        if (want > 32) want = 32;
                        if (!dst->downloaded && ai < want) {
                            (void)download_queue_artwork(ch->channel_id, post->post_id, dst, DOWNLOAD_PRIORITY_LOW);
                        }
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

    ESP_LOGI(TAG, "Updated channel index: %zu total entries", all_count);
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

    ESP_LOGI(TAG, "Eviction needed: %zu downloaded files exceed limit of %zu", 
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

    ESP_LOGI(TAG, "Evicted %zu artwork files to stay within limit of %zu", 
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
    
    ESP_LOGI(TAG, "Refresh task started for channel %s", ch->channel_id);
    
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
    const uint32_t REFRESH_INTERVAL_SEC = 3600;
    
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
        
        // Query posts until we have TARGET_COUNT or no more available
        while (total_queried < TARGET_COUNT && ch->refreshing) {
            memset(resp, 0, sizeof(makapix_query_response_t));
            esp_err_t err = makapix_api_query_posts(&query_req, resp);
            
            if (err != ESP_OK || !resp->success) {
                ESP_LOGW(TAG, "Query failed: %s", resp->error);
                break;
            }
            
            if (resp->post_count == 0) {
                ESP_LOGI(TAG, "No more posts available");
                break;
            }
            
            // Update channel index with new posts
            update_index_bin(ch, resp->posts, resp->post_count);

            // Queue background downloads for artworks ahead in play order
            makapix_channel_ensure_downloads_ahead((channel_handle_t)ch, 16, NULL);

            // If Live Mode is active, mark schedule dirty
            if (ch->navigator_ready && ch->navigator.live_mode) {
                play_navigator_mark_live_dirty(&ch->navigator);
            }

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
            
            if (!resp->has_more) break;
            
            // Delay between queries
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        free(resp);
        
        // Evict excess artworks
        evict_excess_artworks(ch, TARGET_COUNT);
        
        // Save metadata
        time_t now = time(NULL);
        save_channel_metadata(ch, query_req.has_cursor ? query_req.cursor : "", now);
        ch->last_refresh_time = now;
        
        ESP_LOGI(TAG, "Refresh cycle completed: queried %zu posts, channel has %zu entries", 
                 total_queried, ch->entry_count);
        
        // Wait 1 hour before next refresh
        for (uint32_t elapsed = 0; elapsed < REFRESH_INTERVAL_SEC && ch->refreshing; elapsed++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        if (!ch->refreshing) {
            break;
        }
    }
    
    ESP_LOGI(TAG, "Refresh task exiting");
    ch->refreshing = false;
    ch->refresh_task = NULL;
    vTaskDelete(NULL);
}

