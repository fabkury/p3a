// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "content_service.h"
#include "channel_cache.h"

esp_err_t content_service_init(void)
{
    return channel_cache_init();
}
