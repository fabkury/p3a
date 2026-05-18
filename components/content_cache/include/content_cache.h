// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file content_cache.h
 * @brief Content cache public interface for download state and channel management
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char storage_key[64];
    char filepath[256];
    bool is_cached;
    bool is_downloading;
    int download_progress;
} cached_item_t;

esp_err_t content_cache_init(void);
void content_cache_deinit(void);
bool content_cache_is_busy(void);
bool content_cache_get_active_channel(char *out_channel_id, size_t max_len);
void content_cache_set_channels(const char **channel_ids, size_t count);
void content_cache_reset_cursors(void);
void content_cache_rescan(void);

#ifdef __cplusplus
}
#endif
