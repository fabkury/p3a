// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file channel_cache_internal.h
 * @brief Internal hash table helpers for channel_cache split files
 */

#ifndef CHANNEL_CACHE_INTERNAL_H
#define CHANNEL_CACHE_INTERNAL_H

#include "channel_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internal hash-table helpers shared across channel_cache split files.
 *
 * These are NOT part of the public API — only used by channel_cache*.c files.
 */

void ci_hash_free(channel_cache_t *cache);
void ci_hash_add_entry(channel_cache_t *cache, uint32_t ci_index);
void ci_rebuild_hash_tables(channel_cache_t *cache);
void lai_hash_free(channel_cache_t *cache);
void lai_rebuild_hash(channel_cache_t *cache);

/**
 * @brief Build vault file path for a Makapix-style entry (kind=artwork).
 *
 * Uses the storage_key_uuid + SHA256 sharding scheme:
 *   {vault_path}/{sha[0]}/{sha[1]}/{sha[2]}/{storage_key}.{ext}
 *
 * Caller is responsible for filtering on entry->kind == artwork; passing a
 * playlist entry will produce a path derived from a zeroed UUID.
 */
void build_vault_path_from_entry(const makapix_channel_entry_t *entry,
                                 const char *vault_path,
                                 char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_CACHE_INTERNAL_H
