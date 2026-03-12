// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "channel_cache.h"
#include "channel_cache_internal.h"
#include "freertos/task.h"
#include "makapix_channel_internal.h"
#include "makapix_channel_utils.h"
#include "playlist_manager.h"
#include "play_scheduler.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "channel_cache";

/**
 * @brief Parse ISO8601 UTC timestamp to Unix time
 * (Already defined in makapix_channel_utils.c, forward declare here)
 */
extern time_t parse_iso8601_utc(const char *s);

/**
 * @brief Detect file extension from URL
 * (Already defined in makapix_channel_utils.c, forward declare here)
 */
extern file_extension_t detect_file_type(const char *url);

// ============================================================================
// Merge Helpers
// ============================================================================

/**
 * @brief Build vault path from entry without needing makapix_channel_t
 */
static void build_vault_path_from_entry(const makapix_channel_entry_t *entry,
                                        const char *vault_path,
                                        char *out, size_t out_len)
{
    // Convert stored bytes back to UUID string
    char storage_key[40];
    bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

    uint8_t sha256[32];
    if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
        // Best-effort fallback (should never happen)
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
 * @brief Helper: comparison function for qsort (entries by created_at, oldest first)
 */
static int compare_entries_by_created_at(const void *a, const void *b)
{
    const makapix_channel_entry_t *ea = (const makapix_channel_entry_t *)a;
    const makapix_channel_entry_t *eb = (const makapix_channel_entry_t *)b;
    if (ea->created_at < eb->created_at) return -1;
    if (ea->created_at > eb->created_at) return 1;
    return 0;
}

/**
 * @brief Helper: comparison function for qsort (entries by created_at, newest first)
 */
static int compare_entries_by_created_at_desc(const void *a, const void *b)
{
    const makapix_channel_entry_t *ea = (const makapix_channel_entry_t *)a;
    const makapix_channel_entry_t *eb = (const makapix_channel_entry_t *)b;
    // Descending: newer entries first
    if (ea->created_at > eb->created_at) return -1;
    if (ea->created_at < eb->created_at) return 1;
    return 0;
}

// ============================================================================
// Batch Operations (for Makapix refresh)
// ============================================================================

esp_err_t channel_cache_merge_posts(channel_cache_t *cache,
                                    const makapix_post_t *posts,
                                    size_t count,
                                    const char *channels_path,
                                    const char *vault_path)
{
    if (!cache || !posts || count == 0 || !channels_path || !vault_path) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    esp_err_t ret = ESP_OK;

    // Allocate combined array for existing + new entries
    size_t max_count = cache->entry_count + count;
    makapix_channel_entry_t *all_entries = psram_malloc(max_count * sizeof(makapix_channel_entry_t));
    if (!all_entries) {
        xSemaphoreGive(cache->mutex);
        return ESP_ERR_NO_MEM;
    }

    // Copy existing entries
    size_t all_count = cache->entry_count;
    if (cache->entries && cache->entry_count > 0) {
        memcpy(all_entries, cache->entries, cache->entry_count * sizeof(makapix_channel_entry_t));
    } else {
        all_count = 0;
    }

    // Process each new post, yielding periodically so higher-priority tasks
    // (e.g. display rendering) are not starved during large merges.
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && (i % 8) == 0) {
            taskYIELD();
        }
        const makapix_post_t *post = &posts[i];

        // Find existing entry by (post_id, kind)
        int found_idx = -1;
        uint8_t entry_kind = (post->kind == MAKAPIX_POST_KIND_PLAYLIST) ?
                             MAKAPIX_INDEX_POST_KIND_PLAYLIST :
                             MAKAPIX_INDEX_POST_KIND_ARTWORK;

        for (size_t j = 0; j < all_count; j++) {
            if (all_entries[j].post_id == post->post_id && all_entries[j].kind == entry_kind) {
                found_idx = (int)j;
                break;
            }
        }

        // Build new entry
        makapix_channel_entry_t tmp = {0};
        tmp.post_id = post->post_id;
        tmp.kind = entry_kind;
        tmp.created_at = (uint32_t)parse_iso8601_utc(post->created_at);
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
            tmp.total_artworks = 0;
        } else if (post->kind == MAKAPIX_POST_KIND_PLAYLIST) {
            tmp.extension = 0;
            tmp.artwork_modified_at = 0;
            tmp.total_artworks = post->total_artworks;
            memset(tmp.storage_key_uuid, 0, sizeof(tmp.storage_key_uuid));

            // Best-effort: write/update playlist cache on disk
            playlist_metadata_t playlist = {0};
            playlist.post_id = post->post_id;
            playlist.total_artworks = post->total_artworks;
            playlist.loaded_artworks = 0;
            playlist.available_artworks = 0;

            if (post->artworks_count > 0 && post->artworks) {
                playlist.artworks = psram_calloc(post->artworks_count, sizeof(artwork_ref_t));
                if (playlist.artworks) {
                    playlist.loaded_artworks = (int32_t)post->artworks_count;
                    for (size_t ai = 0; ai < post->artworks_count; ai++) {
                        const makapix_artwork_t *src = &post->artworks[ai];
                        artwork_ref_t *dst = &playlist.artworks[ai];
                        memset(dst, 0, sizeof(*dst));
                        dst->post_id = src->post_id;
                        strncpy(dst->storage_key, src->storage_key, sizeof(dst->storage_key) - 1);
                        strlcpy(dst->art_url, src->art_url, sizeof(dst->art_url));
                        dst->artwork_modified_at = parse_iso8601_utc(src->artwork_modified_at);

                        // Determine type from URL extension
                        switch (detect_file_type(src->art_url)) {
                            case EXT_WEBP: dst->type = ASSET_TYPE_WEBP; break;
                            case EXT_GIF:  dst->type = ASSET_TYPE_GIF;  break;
                            case EXT_PNG:  dst->type = ASSET_TYPE_PNG;  break;
                            case EXT_JPEG: dst->type = ASSET_TYPE_JPEG; break;
                            default:       dst->type = ASSET_TYPE_WEBP; break;
                        }

                        // Build vault path for the artwork
                        uint8_t art_uuid[16];
                        if (uuid_to_bytes(src->storage_key, art_uuid)) {
                            makapix_channel_entry_t art_entry = {0};
                            memcpy(art_entry.storage_key_uuid, art_uuid, 16);
                            art_entry.extension = detect_file_type(src->art_url);
                            build_vault_path_from_entry(&art_entry, vault_path, dst->filepath, sizeof(dst->filepath));

                            struct stat st;
                            dst->downloaded = (stat(dst->filepath, &st) == 0);
                        }
                    }

                    // Count available artworks
                    int32_t cnt = 0;
                    for (int32_t j = 0; j < playlist.loaded_artworks; j++) {
                        if (playlist.artworks[j].downloaded) cnt++;
                    }
                    playlist.available_artworks = cnt;
                }
            }

            // Save and free temporary playlist structure
            playlist_save_to_disk(&playlist);
            free(playlist.artworks);
        } else {
            continue;  // Unknown kind
        }

        if (found_idx >= 0) {
            // Existing entry - check for artwork file updates
            if (post->kind == MAKAPIX_POST_KIND_ARTWORK) {
                uint32_t old_artwork_modified = all_entries[(size_t)found_idx].artwork_modified_at;
                uint32_t new_artwork_modified = tmp.artwork_modified_at;

                // If artwork file timestamp changed, delete local file to trigger re-download
                if (old_artwork_modified != 0 && new_artwork_modified != 0 &&
                    old_artwork_modified != new_artwork_modified) {

                    char file_path[512];
                    build_vault_path_from_entry(&all_entries[(size_t)found_idx], vault_path,
                                                file_path, sizeof(file_path));

                    struct stat st;
                    if (stat(file_path, &st) == 0) {
                        ESP_LOGD(TAG, "Artwork file updated on server (post_id=%ld), deleting local copy",
                                (long)post->post_id);
                        if (unlink(file_path) == 0) {
                            // Also remove from LAi
                            lai_post_id_node_t *node;
                            HASH_FIND_INT(cache->lai_hash, &post->post_id, node);
                            if (node) {
                                HASH_DEL(cache->lai_hash, node);
                                free(node);
                                // Also remove from array
                                for (size_t k = 0; k < cache->available_count; k++) {
                                    if (cache->available_post_ids[k] == post->post_id) {
                                        cache->available_post_ids[k] = cache->available_post_ids[--cache->available_count];
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Update the entry with new metadata
            all_entries[(size_t)found_idx] = tmp;
        } else {
            // New entry - add it
            all_entries[all_count++] = tmp;
        }
    }

    // Enforce CHANNEL_CACHE_MAX_ENTRIES limit after deduplication
    if (all_count > CHANNEL_CACHE_MAX_ENTRIES) {
        ESP_LOGI(TAG, "Ci exceeds limit (%zu > %zu), truncating oldest entries",
                 all_count, (size_t)CHANNEL_CACHE_MAX_ENTRIES);

        // Sort by created_at descending (newest first)
        qsort(all_entries, all_count, sizeof(makapix_channel_entry_t),
              compare_entries_by_created_at_desc);

        // Evict entries beyond the limit (oldest ones, now at the end)
        for (size_t i = CHANNEL_CACHE_MAX_ENTRIES; i < all_count; i++) {
            int32_t post_id = all_entries[i].post_id;

            // Check if file is downloaded (in LAi)
            lai_post_id_node_t *node;
            HASH_FIND_INT(cache->lai_hash, &post_id, node);
            if (node) {
                // File exists - delete it
                char file_path[512];
                build_vault_path_from_entry(&all_entries[i], vault_path, file_path, sizeof(file_path));
                unlink(file_path);

                // Remove from LAi hash
                HASH_DEL(cache->lai_hash, node);
                free(node);

                // Remove from LAi array
                for (size_t k = 0; k < cache->available_count; k++) {
                    if (cache->available_post_ids[k] == post_id) {
                        cache->available_post_ids[k] = cache->available_post_ids[--cache->available_count];
                        break;
                    }
                }
            }
        }

        all_count = CHANNEL_CACHE_MAX_ENTRIES;
    }

    // Update cache entries
    free(cache->entries);
    cache->entries = all_entries;
    cache->entry_count = all_count;

    // Rebuild hash table
    ci_rebuild_hash_tables(cache);

    // Ensure available_post_ids array exists and is large enough for LAi operations
    if (cache->entry_count > 0) {
        if (!cache->available_post_ids) {
            // First allocation
            cache->available_post_ids = psram_malloc(cache->entry_count * sizeof(int32_t));
            if (cache->available_post_ids) {
                cache->available_capacity = cache->entry_count;
                cache->available_count = 0;  // Stays 0 until files are downloaded
                ESP_LOGD(TAG, "Allocated available_post_ids for '%s' (capacity: %zu)",
                         cache->channel_id, cache->entry_count);
            }
        } else if (cache->available_capacity < cache->entry_count) {
            // Grow: entry_count increased beyond current capacity (e.g., cache size config change)
            int32_t *new_arr = psram_malloc(cache->entry_count * sizeof(int32_t));
            if (new_arr) {
                if (cache->available_count > 0) {
                    memcpy(new_arr, cache->available_post_ids,
                           cache->available_count * sizeof(int32_t));
                }
                free(cache->available_post_ids);
                cache->available_post_ids = new_arr;
                ESP_LOGI(TAG, "Grew available_post_ids for '%s': %zu -> %zu",
                         cache->channel_id, cache->available_capacity, cache->entry_count);
                cache->available_capacity = cache->entry_count;
            } else {
                {
                    char _dn[64];
                    ps_get_display_name(cache->channel_id, _dn, sizeof(_dn));
                    ESP_LOGE(TAG, "Failed to grow available_post_ids for '%s'", _dn);
                }
            }
        }
    }

    ESP_LOGD(TAG, "Merged %zu posts into cache '%s': total %zu entries",
             count, cache->channel_id, all_count);

    // Release mutex before scheduling I/O to avoid blocking other threads
    xSemaphoreGive(cache->mutex);

    // Defer save via debounce timer to avoid blocking SD card I/O on every
    // MQTT response.  The merged data is safely in RAM; the periodic flush
    // (15 s debounce / 120 s max delay) will persist it to disk.
    channel_cache_schedule_save(cache);

    return ret;
}
