// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_channel_refresh.c
 * @brief Makapix channel server refresh: paginated queries and cache update
 */

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
#include "play_scheduler.h" // For ps_get_display_name()
#include "sntp_sync.h"      // For sntp_sync_is_synchronized()
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
            strlcpy(out_cursor, meta.cursor, sizeof(meta.cursor));
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

    // Hold the cache lifecycle lock around lookup + merge so the scheduler's
    // free loop can't pull the cache out from under us mid-merge. (Without this,
    // a concurrent playset switch's channel_cache_free() can land between the
    // registry_find and the merge_posts, and merge_posts ends up writing into
    // freed memory — corrupting whatever the heap has handed out next.)
    channel_cache_lifecycle_lock();
    channel_cache_t *cache = channel_cache_registry_find(ch->channel_id);
    if (!cache) {
        ESP_LOGW(TAG, "Cache not registered for channel '%s', skipping batch", ch->base.name);
        channel_cache_lifecycle_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = channel_cache_merge_posts(cache, posts, count, ch->channels_path, ch->vault_path);
    channel_cache_lifecycle_unlock();
    return err;
}

// ============================================================================
// Full-refresh eviction
// ============================================================================

/**
 * @brief Si hash node — tracks post_ids seen during the current refresh cycle.
 */
typedef struct {
    int32_t post_id;
    UT_hash_handle hh;
} si_node_t;

/**
 * @brief Evict cache entries not present in the Si hash (full-refresh model).
 *
 * Compacts cache->entries[] in place, keeping only entries whose post_id is
 * in si_hash. Evicted artwork entries have their vault file unlinked. The
 * evicted post_ids are removed from this channel's LAi; sibling channels'
 * stale LAi is corrected lazily by the existing pre-swap stat() defense in
 * play_scheduler_navigation.c. Mirrors giphy_evict_orphans() in
 * components/giphy/giphy_refresh.c.
 */
static void makapix_evict_orphans(channel_cache_t *cache,
                                  si_node_t *si_hash,
                                  const char *vault_path)
{
    if (!cache || !vault_path) return;

    int32_t *evicted_ids = NULL;
    size_t evicted = 0;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    if (cache->entry_count > 0) {
        evicted_ids = psram_malloc(cache->entry_count * sizeof(int32_t));
    }

    static const char *ext_strings[] = {".webp", ".gif", ".png", ".jpg"};
    size_t kept = 0;
    char file_path[256];

    for (size_t i = 0; i < cache->entry_count; i++) {
        si_node_t *found = NULL;
        HASH_FIND_INT(si_hash, &cache->entries[i].post_id, found);
        if (found) {
            // Keep — compact in place
            if (kept != i) {
                memcpy(&cache->entries[kept], &cache->entries[i],
                       sizeof(makapix_channel_entry_t));
            }
            kept++;
        } else {
            // Evict — unlink vault file (artworks only) and record post_id
            const makapix_channel_entry_t *entry = &cache->entries[i];

            if (entry->kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
                char uuid_str[37];
                bytes_to_uuid(entry->storage_key_uuid, uuid_str, sizeof(uuid_str));

                uint8_t sha256[32];
                if (storage_key_sha256(uuid_str, sha256) == ESP_OK) {
                    int ext_idx = (entry->extension < 4) ? entry->extension : 0;
                    snprintf(file_path, sizeof(file_path),
                             "%s/%02x/%02x/%02x/%s%s",
                             vault_path,
                             (unsigned int)sha256[0],
                             (unsigned int)sha256[1],
                             (unsigned int)sha256[2],
                             uuid_str, ext_strings[ext_idx]);
                    unlink(file_path);
                }
            }

            if (evicted_ids) {
                evicted_ids[evicted] = entry->post_id;
            }
            evicted++;
        }
    }

    cache->entry_count = kept;

    // Rebuild Ci hash (post_id_hash) from compacted entries — indices changed.
    // Inline rather than calling ci_rebuild_hash_tables so we keep the work
    // inside the cache mutex without pulling in channel_cache_internal.h.
    ci_post_id_node_t *cnode, *ctmp;
    HASH_ITER(hh, cache->post_id_hash, cnode, ctmp) {
        HASH_DEL(cache->post_id_hash, cnode);
        free(cnode);
    }
    cache->post_id_hash = NULL;

    for (size_t i = 0; i < kept; i++) {
        ci_post_id_node_t *n = psram_malloc(sizeof(ci_post_id_node_t));
        if (n) {
            n->post_id = cache->entries[i].post_id;
            n->ci_index = (uint32_t)i;
            HASH_ADD_INT(cache->post_id_hash, post_id, n);
        }
    }

    cache->dirty = true;
    xSemaphoreGive(cache->mutex);

    // LAi cleanup outside the cache mutex — lai_remove_entry locks internally.
    for (size_t i = 0; i < evicted; i++) {
        lai_remove_entry(cache, evicted_ids[i]);
    }
    free(evicted_ids);

    channel_cache_schedule_save(cache);

    ESP_LOGI(TAG, "Full refresh: evicted %zu orphaned entries, %zu kept (channel '%s')",
             evicted, kept, cache->display_name);
}

// Park the task at a sync point so the reaper can delete it externally.
// Must be the only path the refresh task takes to exit. Does not return.
static void park_and_wait_for_reap(makapix_channel_t *ch)
{
    ch->refreshing = false;
    if (ch->refresh_parked_sem) {
        xSemaphoreGive(ch->refresh_parked_sem);
    }
    vTaskSuspend(NULL);
    // Defensive: if some future code path resumes us, exit cleanly rather than
    // returning into FreeRTOS task-function-returned undefined behavior.
    vTaskDelete(NULL);
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
        park_and_wait_for_reap(ch);
        return;
    }
    
    // Build a user-friendly channel name from spec fields for UI display
    char display_name[64];
    if (strcmp(ch->channel_key, "all") == 0) {
        strlcpy(display_name, "All Artworks", sizeof(display_name));
    } else if (strcmp(ch->channel_key, "promoted") == 0) {
        strlcpy(display_name, "Promoted", sizeof(display_name));
    } else if (strcmp(ch->channel_key, "user") == 0) {
        strlcpy(display_name, "My Channel", sizeof(display_name));
    } else if (strcmp(ch->channel_key, "by_user") == 0) {
        snprintf(display_name, sizeof(display_name), "User: %.48s", ch->identifier);
    } else if (strcmp(ch->channel_key, "reactions") == 0) {
        snprintf(display_name, sizeof(display_name), "Reactions: %.48s", ch->identifier);
    } else if (strcmp(ch->channel_key, "hashtag") == 0) {
        snprintf(display_name, sizeof(display_name), "#%.56s", ch->identifier);
    } else {
        snprintf(display_name, sizeof(display_name), "Channel: %.52s", ch->channel_key);
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
    
    // Determine channel type from stored spec fields
    makapix_channel_type_t channel_type = MAKAPIX_CHANNEL_ALL;
    makapix_query_request_t query_req = {0};

    if (strcmp(ch->channel_key, "all") == 0) {
        channel_type = MAKAPIX_CHANNEL_ALL;
    } else if (strcmp(ch->channel_key, "promoted") == 0) {
        channel_type = MAKAPIX_CHANNEL_PROMOTED;
    } else if (strcmp(ch->channel_key, "user") == 0) {
        channel_type = MAKAPIX_CHANNEL_USER;
    } else if (strcmp(ch->channel_key, "by_user") == 0) {
        channel_type = MAKAPIX_CHANNEL_BY_USER;
        strlcpy(query_req.user_sqid, ch->identifier, sizeof(query_req.user_sqid));
    } else if (strcmp(ch->channel_key, "reactions") == 0) {
        channel_type = MAKAPIX_CHANNEL_REACTIONS;
        strlcpy(query_req.user_sqid, ch->identifier, sizeof(query_req.user_sqid));
    } else if (strcmp(ch->channel_key, "hashtag") == 0) {
        channel_type = MAKAPIX_CHANNEL_HASHTAG;
        strlcpy(query_req.hashtag, ch->identifier, sizeof(query_req.hashtag));
    } else if (ch->channel_key[0] == '\0') {
        // No spec set (e.g. single artwork channel) - skip refresh
        park_and_wait_for_reap(ch);
        return;
    }
    
    query_req.channel = channel_type;
    query_req.sort = MAKAPIX_SORT_SERVER_ORDER;
    query_req.limit = 32;
    query_req.has_cursor = false;

    const size_t TARGET_COUNT = config_store_get_channel_cache_size();
    
    while (ch->refreshing) {
        // Check for PICO-8 mode - exit refresh cycle cleanly
        if (p3a_state_get() == P3A_STATE_PICO8_STREAMING) {
            ESP_LOGI(TAG, "PICO-8 mode active, exiting refresh cycle");
            break;
        }

        size_t total_queried = 0;
        bool query_succeeded = false;
        query_req.has_cursor = false;
        query_req.cursor[0] = '\0';

        // Si hash: every post_id the server returns this cycle. After a
        // successful complete walk, makapix_evict_orphans removes cache
        // entries whose post_id is not in Si — that is how deleted, hidden,
        // or banned artworks leave the local cache. Capped at TARGET_COUNT
        // to bound memory. Mirrors components/giphy/giphy_refresh.c.
        si_node_t *si_hash = NULL;
        size_t si_count = 0;

        // Full-refresh model: walk from the top of the channel each cycle
        // (no saved cursor). Eviction at the end of the cycle is what makes
        // the cache converge to the server's current set.
        
        // Response buffer is ~25 KB. Prefer PSRAM so concurrent refreshes don't
        // pin internal RAM that the SDMMC driver needs for its DMA bounce
        // buffers; psram_malloc() falls back to internal if PSRAM is exhausted.
        makapix_query_response_t *resp = psram_malloc(sizeof(makapix_query_response_t));
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

            query_succeeded = true;

            if (resp->post_count == 0) {
                ESP_LOGD(TAG, "No more posts available");
                break;
            }

            // Merge batch into channel cache
            esp_err_t merge_err = merge_refresh_batch(ch, resp->posts, resp->post_count);
            if (merge_err == ESP_OK) {
                channel_cache_lifecycle_lock();
                channel_cache_t *diag_cache = channel_cache_registry_find(ch->channel_id);
                size_t diag_entry = diag_cache ? diag_cache->entry_count : 0;
                size_t diag_avail = diag_cache ? diag_cache->available_count : 0;
                channel_cache_lifecycle_unlock();
                char _dn[64];
                ps_get_display_name(ch->channel_id, _dn, sizeof(_dn));
                ESP_LOGI(TAG, "Batch merged: ch='%s' entry_count=%zu available=%zu",
                         _dn, diag_entry, diag_avail);
            } else {
                char _dn[64];
                ps_get_display_name(ch->channel_id, _dn, sizeof(_dn));
                ESP_LOGW(TAG, "Batch merge failed: ch='%s' err=%s",
                         _dn, esp_err_to_name(merge_err));
            }

            // Check for shutdown after index update
            if (!ch->refreshing) {
                ESP_LOGI(TAG, "Shutdown requested during index update, exiting");
                break;
            }

            // Signal download manager to rescan - new index entries arrived
            download_manager_rescan();

            // Track each post_id seen this cycle for end-of-cycle eviction.
            // Cap at TARGET_COUNT so the post-eviction cache never exceeds
            // the configured limit. Mirrors giphy_refresh.c lines 482-495.
            for (size_t pi = 0; pi < resp->post_count && si_count < (size_t)TARGET_COUNT; pi++) {
                int32_t pid = resp->posts[pi].post_id;
                si_node_t *existing = NULL;
                HASH_FIND_INT(si_hash, &pid, existing);
                if (!existing) {
                    si_node_t *n = psram_malloc(sizeof(si_node_t));
                    if (n) {
                        n->post_id = pid;
                        HASH_ADD_INT(si_hash, post_id, n);
                        si_count++;
                    }
                }
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
                ESP_LOGI(TAG, "Cursor advanced: len=%zu, preview=%.20s...", copy_len, query_req.cursor);
            } else {
                query_req.has_cursor = false;
                if (resp->has_more) {
                    channel_cache_lifecycle_lock();
                    channel_cache_t *diag_cache = channel_cache_registry_find(ch->channel_id);
                    size_t diag_entry = diag_cache ? diag_cache->entry_count : 0;
                    channel_cache_lifecycle_unlock();
                    ESP_LOGW(TAG, "has_more=true but next_cursor is empty! Pagination may be broken. "
                             "total_queried=%zu, entry_count=%zu",
                             total_queried, diag_entry);
                }
            }
            
            if (!resp->has_more) {
                break;
            }
            
            if (!first_query_completed && total_queried > 0) {
                first_query_completed = true;
            }
            
            // Delay between queries
            vTaskDelay(pdMS_TO_TICKS(350));
        }
        
        free(resp);

        // Check for shutdown BEFORE saving metadata — a cancelled refresh must not
        // update the timestamp, otherwise the freshness check will skip the next refresh.
        if (!ch->refreshing) {
            // Free Si hash on cancel — eviction is gated on ch->refreshing
            // so it won't run, but we still own the allocation.
            si_node_t *cnode_si, *ctmp_si;
            HASH_ITER(hh, si_hash, cnode_si, ctmp_si) {
                HASH_DEL(si_hash, cnode_si);
                free(cnode_si);
            }
            si_hash = NULL;

            ESP_LOGW(TAG, "Refresh cancelled for channel '%s', not updating refresh timestamp",
                     ch->channel_id);
            // Signal Play Scheduler so refresh_async_pending gets cleared.
            // Without this, a cancelled refresh leaves an orphan entry in
            // s_ps_pending_refreshes that never matches by channel_id once
            // the playset switch has rebound the slot.
            makapix_channel_signal_refresh_done();
            makapix_ps_refresh_mark_complete(ch->channel_id);
            break;
        }

        // Eviction: remove cache entries the server didn't return this cycle.
        // Must run BEFORE the cache flush below so the on-disk state reflects
        // the post-eviction cache. Re-resolve the cache via the lifecycle lock
        // since the pointer captured by merge_refresh_batch's earlier callers
        // is stale by construction across MQTT waits. Mirrors
        // components/giphy/giphy_refresh.c lines 521-531.
        if (query_succeeded && si_hash) {
            channel_cache_lifecycle_lock();
            channel_cache_t *evict_cache = channel_cache_registry_find(ch->channel_id);
            if (evict_cache) {
                makapix_evict_orphans(evict_cache, si_hash, ch->vault_path);
            }
            channel_cache_lifecycle_unlock();
        }

        // Free Si hash unconditionally (whether eviction ran or not).
        {
            si_node_t *node, *tmp;
            HASH_ITER(hh, si_hash, node, tmp) {
                HASH_DEL(si_hash, node);
                free(node);
            }
            si_hash = NULL;
        }

        if (query_succeeded) {
            // Flush dirty cache to disk BEFORE saving the metadata timestamp.
            // Both files live on the SD card but are written independently; if
            // power is lost after the metadata (.json) is saved but before the
            // cache (.cache) is flushed, the device would boot with a "fresh"
            // timestamp yet stale/incomplete cache data — skipping the refresh
            // it actually needs.  Flushing the cache first guarantees the .cache
            // on disk is at least as recent as the .json timestamp.
            channel_cache_lifecycle_lock();
            channel_cache_t *flush_cache = channel_cache_registry_find(ch->channel_id);
            esp_err_t flush_err = ESP_OK;
            if (flush_cache) {
                flush_err = channel_cache_flush_one(flush_cache, ch->channels_path);
            }
            channel_cache_lifecycle_unlock();
            if (flush_cache && flush_err != ESP_OK) {
                ESP_LOGW(TAG, "Cache flush failed for '%s', skipping metadata save",
                         ch->channel_id);
                break;
            }

            // Save metadata (only persist a valid timestamp if SNTP is synchronized).
            // Cursor is always empty under the full-refresh model — every cycle
            // walks from the top of the channel.
            time_t refresh_ts = sntp_sync_is_synchronized() ? time(NULL) : 0;
            save_channel_metadata(ch, "", refresh_ts);
            ch->last_refresh_time = refresh_ts;
        } else {
            ESP_LOGW(TAG, "Refresh failed for channel '%s', preserving previous refresh timestamp",
                     ch->channel_id);
        }

        // Count how many are artworks vs playlists using cache lookup
        {
            channel_cache_lifecycle_lock();
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
            channel_cache_lifecycle_unlock();
            ESP_LOGD(TAG, "Channel %s: %zu entries (%zu artworks)", ch->base.name, entry_count, artwork_count);
        }

        // Signal that refresh has completed - this unblocks download manager
        makapix_channel_signal_refresh_done();

        // Mark the work loop done BEFORE notifying Play Scheduler. The
        // ps_refresh task observes mark_complete and may immediately try to
        // reap us; if it sees ch->refreshing still true at that point,
        // reap_refresh_task signals the global shutdown bit, which can abort
        // other refresh tasks that are still in their MQTT-wait phase.
        ch->refreshing = false;

        // Signal Play Scheduler that this channel's refresh is complete
        makapix_ps_refresh_mark_complete(ch->channel_id);

        break;  // Exit after single cycle — Play Scheduler handles re-scheduling
    }

    park_and_wait_for_reap(ch);
}

