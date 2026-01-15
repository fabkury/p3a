// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "content_cache.h"
#include "download_manager.h"

esp_err_t content_cache_init(void)
{
    return download_manager_init();
}

void content_cache_deinit(void)
{
    download_manager_deinit();
}

bool content_cache_is_busy(void)
{
    return download_manager_is_busy();
}

bool content_cache_get_active_channel(char *out_channel_id, size_t max_len)
{
    return download_manager_get_active_channel(out_channel_id, max_len);
}

void content_cache_set_channels(const char **channel_ids, size_t count)
{
    download_manager_set_channels(channel_ids, count);
}

void content_cache_reset_cursors(void)
{
    download_manager_reset_cursors();
}

void content_cache_reset_playback_initiated(void)
{
    download_manager_reset_playback_initiated();
}

void content_cache_signal_work_available(void)
{
    download_manager_signal_work_available();
}
