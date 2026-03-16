// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file content_service.c
 * @brief Thin service facade for channel cache initialization
 */

#include "content_service.h"
#include "channel_cache.h"

esp_err_t content_service_init(void)
{
    return channel_cache_init();
}
