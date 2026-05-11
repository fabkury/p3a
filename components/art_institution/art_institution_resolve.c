// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution_resolve.c
 * @brief One-entry-per-call resolver loop for museums that need
 *        on-demand IIIF id resolution (Rijksmuseum's 3-hop Linked Art
 *        walk being the only example today).
 *
 * Wired into the download_task in components/channel_manager/
 * download_manager.c: once per loop iteration we ask the resolver to
 * walk one pending entry. The resolver's HTTP work is short (a few
 * hundred ms) but unbounded enough that we don't want to drain all
 * pending entries before any new downloads can happen — interleaving
 * keeps the device responsive while a Rijks channel's first batch
 * lands.
 *
 * Failure handling matches the design's three-strikes rule
 * (§9.2 / §11): we bump resolve_fails on each failure and tombstone
 * (extension = 0xFE) at 3, after which the download manager skips
 * the entry forever — until the next refresh re-adds the HMO with a
 * fresh budget.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "art_institution_types.h"
#include "channel_cache.h"
#include "play_scheduler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <string.h>

static const char *TAG = "ai_resolve";

#define RESOLVE_MAX_FAILS 3

/**
 * @brief Snapshot one pending unresolved entry from a channel cache
 *
 * Walks cache->entries under the cache mutex and copies out the first
 * entry whose extension == 0xFF and resolve_fails < RESOLVE_MAX_FAILS.
 * Returns true if such an entry was found. The caller uses post_id to
 * locate the entry again after the network walk completes.
 */
static bool snapshot_pending(channel_cache_t *cache,
                             institution_channel_entry_t *out_entry)
{
    if (!cache || !out_entry) return false;
    bool found = false;
    xSemaphoreTake(cache->mutex, portMAX_DELAY);
    for (size_t i = 0; i < cache->entry_count; i++) {
        const institution_channel_entry_t *e =
            (const institution_channel_entry_t *)&cache->entries[i];
        if (e->extension == 0xFF && e->resolve_fails < RESOLVE_MAX_FAILS) {
            memcpy(out_entry, e, sizeof(*out_entry));
            found = true;
            break;
        }
    }
    xSemaphoreGive(cache->mutex);
    return found;
}

/**
 * @brief Locate an entry by post_id and write the resolved values back
 *
 * The cache may have been mutated (refresh, eviction) during the
 * network walk, so we re-find by post_id rather than trusting the
 * snapshot's array position. If the entry is gone, the work is
 * silently discarded — the next refresh will re-add it if the HMO is
 * still listed.
 */
static void commit_resolution(channel_cache_t *cache,
                              int32_t post_id,
                              const institution_channel_entry_t *resolved)
{
    if (!cache || !resolved) return;
    bool committed = false;
    xSemaphoreTake(cache->mutex, portMAX_DELAY);
    for (size_t i = 0; i < cache->entry_count; i++) {
        institution_channel_entry_t *e =
            (institution_channel_entry_t *)&cache->entries[i];
        if (e->post_id == post_id) {
            // Replace iiif_key + extension + resolve_fails; preserve
            // width/height/created_at/kind from the resolved snapshot
            // (which was copied from the original anyway).
            memcpy(e, resolved, sizeof(*e));
            committed = true;
            break;
        }
    }
    cache->dirty = committed;
    xSemaphoreGive(cache->mutex);
    if (committed) channel_cache_schedule_save(cache);
}

static void commit_failure(channel_cache_t *cache, int32_t post_id, bool tombstone)
{
    if (!cache) return;
    xSemaphoreTake(cache->mutex, portMAX_DELAY);
    for (size_t i = 0; i < cache->entry_count; i++) {
        institution_channel_entry_t *e =
            (institution_channel_entry_t *)&cache->entries[i];
        if (e->post_id == post_id) {
            if (tombstone) {
                e->extension = 0xFE;
                ESP_LOGW(TAG, "Tombstoning entry post_id=%ld (3 walk failures)",
                         (long)post_id);
            } else {
                if (e->resolve_fails < 0xFF) e->resolve_fails++;
            }
            cache->dirty = true;
            break;
        }
    }
    xSemaphoreGive(cache->mutex);
    channel_cache_schedule_save(cache);
}

esp_err_t art_institution_resolve_pending(void)
{
    // Build a snapshot of active channel ids once (cheap; play_scheduler
    // returns pointers to internal storage, but we use them right away).
    const char *active_ids[PS_MAX_CHANNELS];
    size_t active_count = play_scheduler_get_active_channel_ids(active_ids, PS_MAX_CHANNELS);
    if (active_count == 0) return ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < active_count; i++) {
        const char *channel_id = active_ids[i];
        if (!channel_id) continue;
        if (!play_scheduler_is_institution_channel(channel_id)) continue;

        char spec_name[33] = {0};
        if (play_scheduler_get_channel_spec_name(channel_id,
                                                 spec_name, sizeof(spec_name)) != ESP_OK) {
            continue;
        }
        char museum_id[16] = {0};
        char axis_unused[32] = {0};
        if (art_institution_parse_spec(spec_name,
                                       museum_id, sizeof(museum_id),
                                       axis_unused, sizeof(axis_unused)) != ESP_OK) {
            continue;
        }
        const art_institution_museum_t *m = art_institution_find(museum_id);
        if (!m || !m->resolve_entry) continue;  // museum doesn't need resolution

        if (art_institution_is_rate_limited(museum_id)) continue;

        // Pin the cache lifecycle while we snapshot. The lock is dropped
        // before the network walk so a concurrent playset switch can't
        // get stuck behind a multi-second HTTP chain.
        channel_cache_lifecycle_lock();
        channel_cache_t *cache = channel_cache_registry_find(channel_id);
        institution_channel_entry_t entry;
        bool have_pending = (cache && snapshot_pending(cache, &entry));
        channel_cache_lifecycle_unlock();

        if (!have_pending) continue;

        // Run the museum-specific walk. This is HTTP-heavy (Rijks does
        // up to 3 fetches) so we run it outside any mutex.
        int32_t post_id = entry.post_id;
        esp_err_t walk = m->resolve_entry(&entry);

        // Re-lock the cache lifecycle for the writeback. A concurrent
        // refresh may have moved entries around or evicted this one;
        // commit_resolution / commit_failure re-find by post_id.
        channel_cache_lifecycle_lock();
        cache = channel_cache_registry_find(channel_id);
        if (cache) {
            if (walk == ESP_OK) {
                commit_resolution(cache, post_id, &entry);
            } else if (walk == ESP_ERR_INVALID_RESPONSE) {
                // Rate-limited mid-walk — don't count as a failure;
                // we'll retry on the next loop iteration once cooldown
                // clears.
            } else {
                // Real failure: increment fail counter, tombstone at 3.
                bool tombstone = (entry.resolve_fails + 1 >= RESOLVE_MAX_FAILS);
                commit_failure(cache, post_id, tombstone);
            }
        }
        channel_cache_lifecycle_unlock();

        // Only resolve one entry per call so the download_task can keep
        // making progress on regular downloads.
        return (walk == ESP_OK) ? ESP_OK : walk;
    }

    return ESP_ERR_NOT_FOUND;
}
