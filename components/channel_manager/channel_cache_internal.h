// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

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

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_CACHE_INTERNAL_H
