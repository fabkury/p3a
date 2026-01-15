// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "content_source.h"

esp_err_t content_source_init_from_channel(content_source_t *src,
                                           channel_handle_t channel,
                                           const char *source_id)
{
    if (!src || !channel) {
        return ESP_ERR_INVALID_ARG;
    }

    src->channel = channel;
    src->source_id = source_id;
    return ESP_OK;
}

esp_err_t content_source_refresh(content_source_t *src)
{
    if (!src || !src->channel) {
        return ESP_ERR_INVALID_ARG;
    }
    return channel_request_refresh(src->channel);
}

esp_err_t content_source_get_post(content_source_t *src, size_t index, channel_post_t *out)
{
    if (!src || !src->channel || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    return channel_get_post(src->channel, index, out);
}

size_t content_source_get_count(content_source_t *src)
{
    if (!src || !src->channel) {
        return 0;
    }

    channel_stats_t stats = {0};
    if (channel_get_stats(src->channel, &stats) != ESP_OK) {
        return 0;
    }
    return stats.total_items;
}
