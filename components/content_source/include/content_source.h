// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "channel_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *source_id;
    channel_handle_t channel;
} content_source_t;

esp_err_t content_source_init_from_channel(content_source_t *src,
                                           channel_handle_t channel,
                                           const char *source_id);
esp_err_t content_source_refresh(content_source_t *src);
esp_err_t content_source_get_post(content_source_t *src, size_t index, channel_post_t *out);
size_t content_source_get_count(content_source_t *src);

#ifdef __cplusplus
}
#endif
