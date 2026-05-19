// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution_refresh.c
 * @brief Shared merge/evict helpers and the per-spec refresh dispatcher.
 *
 * Each museum adapter implements its own page loop because pagination
 * styles diverge (AIC uses page numbers; M2's Rijks uses cursor tokens),
 * but every adapter goes through the same Ci merge and Si-driven orphan
 * eviction here so the channel_cache_t handling stays in one place.
 *
 * Vault-file deletion is intentionally NOT done in the orphan path: the
 * vault is per-museum and shared across channels (§4.3), so deleting in
 * channel A could yank a file that channel B still references. The
 * design's Mechanism 2 (storage_eviction.c, age-based) catches truly
 * orphaned files. Compare with giphy_evict_orphans, which DOES unlink
 * eagerly — Giphy's vault overlap is rare in practice, museum overlap is
 * common.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "channel_cache.h"
#include "play_scheduler.h"
#include "psram_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <string.h>

static const char *TAG = "ai_refresh";

esp_err_t art_institution_merge_entries(struct channel_cache_s *cache,
                                        const institution_channel_entry_t *new_entries,
                                        size_t new_count,
                                        size_t max_entries)
{
    if (!cache || !new_entries || new_count == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    // Allocate combined array sized to the worst case for this merge.
    size_t max_total = cache->entry_count + new_count;
    if (max_total > max_entries) max_total = max_entries;

    // Cast through makapix_channel_entry_t because that is what
    // channel_cache_t->entries is typed as; all three entry layouts share
    // the same 64-byte slot, so the bytes round-trip cleanly.
    makapix_channel_entry_t *all = psram_malloc(max_total * sizeof(makapix_channel_entry_t));
    if (!all) {
        xSemaphoreGive(cache->mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t all_count = 0;
    if (cache->entries && cache->entry_count > 0) {
        size_t copy_count = cache->entry_count;
        if (copy_count > max_total) copy_count = max_total;
        memcpy(all, cache->entries, copy_count * sizeof(makapix_channel_entry_t));
        all_count = copy_count;
    }

    for (size_t i = 0; i < new_count && all_count < max_total; i++) {
        const institution_channel_entry_t *ne = &new_entries[i];

        bool found = false;
        for (size_t j = 0; j < all_count; j++) {
            // post_id is at offset 0 for all three entry layouts.
            if (all[j].post_id == ne->post_id) {
                // Preserve resolver output across refreshes. Rijks emits
                // unresolved (extension=0xFF, iiif_key=HMO URL) entries
                // that a separate Linked-Art walk later mutates in place
                // to (extension<=3, iiif_key=micrio short id). The next
                // refresh re-emits the unresolved sentinel from the API;
                // overwriting wholesale would destroy the resolver's
                // output and orphan the on-disk files (whose paths derive
                // from iiif_key). Tombstones (0xFE) are still replaced so
                // the next refresh hands them a fresh resolve_fails=0
                // budget, per the header contract.
                institution_channel_entry_t *existing =
                    (institution_channel_entry_t *)&all[j];
                bool new_is_unresolved = (ne->extension == 0xFF);
                bool existing_is_resolved = (existing->extension <= 3);
                if (new_is_unresolved && existing_is_resolved) {
                    existing->created_at = ne->created_at;
                } else {
                    memcpy(&all[j], ne, sizeof(makapix_channel_entry_t));
                }
                found = true;
                break;
            }
        }

        if (!found) {
            memcpy(&all[all_count], ne, sizeof(makapix_channel_entry_t));
            all_count++;
        }
    }

    if (cache->entries) {
        free(cache->entries);
    }
    cache->entries = all;
    cache->entry_count = all_count;

    // Rebuild Ci hash table.
    ci_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->post_id_hash, node, tmp) {
        HASH_DEL(cache->post_id_hash, node);
        free(node);
    }
    cache->post_id_hash = NULL;

    for (size_t i = 0; i < all_count; i++) {
        ci_post_id_node_t *n = psram_malloc(sizeof(ci_post_id_node_t));
        if (n) {
            n->post_id = cache->entries[i].post_id;
            n->ci_index = (uint32_t)i;
            HASH_ADD_INT(cache->post_id_hash, post_id, n);
        }
    }

    cache->dirty = true;
    xSemaphoreGive(cache->mutex);

    channel_cache_schedule_save(cache);
    return ESP_OK;
}

void art_institution_evict_orphans(struct channel_cache_s *cache,
                                   ai_si_node_t *si_hash,
                                   const char *museum_id)
{
    if (!cache || !si_hash || !museum_id) return;

    // Collect evicted post_ids so LAi cleanup can happen outside the mutex
    // (lai_remove_entry locks internally).
    int32_t *evicted_ids = NULL;
    size_t evicted = 0;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    if (cache->entry_count > 0) {
        evicted_ids = psram_malloc(cache->entry_count * sizeof(int32_t));
    }

    size_t kept = 0;
    for (size_t i = 0; i < cache->entry_count; i++) {
        ai_si_node_t *found = NULL;
        HASH_FIND_INT(si_hash, &cache->entries[i].post_id, found);
        if (found) {
            if (kept != i) {
                memcpy(&cache->entries[kept], &cache->entries[i],
                       sizeof(makapix_channel_entry_t));
            }
            kept++;
        } else {
            // Vault file NOT deleted — see file comment.
            if (evicted_ids) {
                evicted_ids[evicted] = cache->entries[i].post_id;
            }
            evicted++;
        }
    }

    cache->entry_count = kept;

    // Rebuild Ci hash.
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

    for (size_t i = 0; i < evicted; i++) {
        int removed_pos = -1;
        if (lai_remove_entry(cache, evicted_ids[i], &removed_pos)) {
            play_scheduler_compensate_cursor_after_lai_remove(cache, removed_pos);
        }
    }
    free(evicted_ids);

    channel_cache_schedule_save(cache);

    ESP_LOGI(TAG, "Full refresh (museum=%s): evicted %zu orphans, %zu kept",
             museum_id, evicted, kept);
}

esp_err_t art_institution_refresh_by_spec(const char *channel_id,
                                          const char *spec_name,
                                          const char *identifier,
                                          uint32_t channel_offset)
{
    if (!channel_id || !spec_name || !identifier) return ESP_ERR_INVALID_ARG;

    char museum_id[16] = {0};
    char axis[32] = {0};
    esp_err_t err = art_institution_parse_spec(spec_name,
                                               museum_id, sizeof(museum_id),
                                               axis, sizeof(axis));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Malformed institution spec_name '%s'", spec_name);
        return err;
    }

    const art_institution_museum_t *m = art_institution_find(museum_id);
    if (!m || !m->refresh_channel) {
        ESP_LOGW(TAG, "Unknown museum id '%s' in spec '%s'", museum_id, spec_name);
        return ESP_ERR_NOT_FOUND;
    }

    return m->refresh_channel(channel_id, axis, identifier, channel_offset);
}
