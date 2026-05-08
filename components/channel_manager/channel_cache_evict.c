// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file channel_cache_evict.c
 * @brief Full-refresh orphan eviction for Makapix-style channel caches
 *
 * Shared implementation called by both the MQTT-driven refresh
 * (makapix_channel_refresh.c) and the HTTPS fallback for the promoted
 * channel (makapix_promoted_https.c). Mirrors giphy_evict_orphans() in
 * components/giphy/giphy_refresh.c for the Makapix entry layout.
 */

#include "channel_cache_evict.h"
#include "channel_cache.h"
#include "channel_cache_internal.h"  // build_vault_path_from_entry, ci_post_id_node_t
#include "psram_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "cache_evict";

void channel_cache_evict_orphans_makapix(channel_cache_t *cache,
                                         channel_cache_si_node_t *si_hash,
                                         const char *vault_path)
{
    if (!cache || !si_hash || !vault_path) return;

    int32_t *evicted_ids = NULL;
    size_t evicted = 0;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    if (cache->entry_count > 0) {
        evicted_ids = psram_malloc(cache->entry_count * sizeof(int32_t));
    }

    size_t kept = 0;
    char file_path[512];

    for (size_t i = 0; i < cache->entry_count; i++) {
        channel_cache_si_node_t *found = NULL;
        HASH_FIND_INT(si_hash, &cache->entries[i].post_id, found);
        if (found) {
            // Keep — compact in place.
            if (kept != i) {
                memcpy(&cache->entries[kept], &cache->entries[i],
                       sizeof(makapix_channel_entry_t));
            }
            kept++;
        } else {
            // Evict — unlink vault file (artworks only) and record post_id
            // so LAi can be cleaned up after we release the cache mutex.
            const makapix_channel_entry_t *entry = &cache->entries[i];

            if (entry->kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
                build_vault_path_from_entry(entry, vault_path,
                                            file_path, sizeof(file_path));
                unlink(file_path);
            }

            if (evicted_ids) {
                evicted_ids[evicted] = entry->post_id;
            }
            evicted++;
        }
    }

    cache->entry_count = kept;

    // Rebuild Ci hash (post_id_hash) from compacted entries — indices changed.
    // Inline rather than calling ci_rebuild_hash_tables so the work stays
    // inside the cache mutex critical section.
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
