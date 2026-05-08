// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file channel_cache_evict.h
 * @brief Full-refresh orphan eviction for Makapix-style channel caches
 *
 * Shared between every refresh path that walks a Makapix channel from the
 * top each cycle (MQTT-driven refresh and the HTTPS fallback for the
 * promoted channel). Both paths build the same Set-of-Ids hash while paging
 * and pass it to channel_cache_evict_orphans_makapix() at the end so the
 * local cache converges to the server's current set of posts.
 */

#ifndef CHANNEL_CACHE_EVICT_H
#define CHANNEL_CACHE_EVICT_H

#include <stdint.h>
#include "channel_cache.h"  // channel_cache_t, uthash, psram allocator macros

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Hash node for the Set-of-Ids (Si) tracked during a full refresh.
 *
 * The caller allocates one node per unique post_id returned by the server
 * during the refresh cycle and inserts it into a uthash via HASH_ADD_INT.
 * Ownership of the hash and its nodes stays with the caller — this module
 * only reads the hash during eviction and never frees nodes.
 */
typedef struct channel_cache_si_node_s {
    int32_t post_id;
    UT_hash_handle hh;
} channel_cache_si_node_t;

/**
 * @brief Evict cache entries whose post_id is not present in the Si hash.
 *
 * Used by the full-refresh model: after walking a Makapix channel the caller
 * passes the Si hash of post_ids the server returned, and this function
 * removes from Ci, LAi, and the SD-card vault any entries that did not
 * appear.
 *
 * Behavior:
 *   - Compacts cache->entries[] in place, keeping only entries whose post_id
 *     is present in si_hash.
 *   - Artwork entries (kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) have their
 *     vault file unlinked. Playlist entries have no on-disk vault file so
 *     their persisted metadata is left untouched.
 *   - Rebuilds Ci's post_id_hash so the index→post_id mapping reflects the
 *     compacted array.
 *   - Removes evicted post_ids from this channel's LAi via lai_remove_entry.
 *     Sibling channels' stale LAi is corrected lazily by the pre-swap stat()
 *     defense in play_scheduler_navigation.c.
 *   - Marks the cache dirty and schedules a debounced save.
 *
 * Thread-safety: takes cache->mutex for the in-place compaction and Ci hash
 * rebuild. LAi cleanup runs after releasing the cache mutex
 * (lai_remove_entry takes its own lock).
 *
 * Defensive: does nothing if any argument is NULL. Callers should still gate
 * on a successful refresh + non-empty si_hash so a cache is never wiped on
 * a network error or empty response.
 *
 * Mirrors giphy_evict_orphans() in components/giphy/giphy_refresh.c.
 *
 * @param cache       Channel cache (Makapix-style entries)
 * @param si_hash     uthash of post_ids seen during the refresh cycle
 * @param vault_path  Base path for vault files (e.g. "/sdcard/p3a/vault")
 */
void channel_cache_evict_orphans_makapix(channel_cache_t *cache,
                                         channel_cache_si_node_t *si_hash,
                                         const char *vault_path);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_CACHE_EVICT_H
