// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file content_service.c
 * @brief Thin service facade for content-source initialization (channel
 *        cache + art-institution rate-limit table).
 */

#include "content_service.h"
#include "channel_cache.h"
#include "art_institution.h"
#include "esp_log.h"

static const char *TAG = "content_svc";

esp_err_t content_service_init(void)
{
    esp_err_t err = channel_cache_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel_cache_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Reset the per-museum rate-limit cooldown table. Failures here are
    // not fatal (the table is static and zero-initialized anyway), but
    // surfacing the result keeps logs consistent.
    esp_err_t ai_err = art_institution_init();
    if (ai_err != ESP_OK) {
        ESP_LOGW(TAG, "art_institution_init failed: %s", esp_err_to_name(ai_err));
    }

    // pin_lists_init is intentionally NOT called here — content_service_init
    // runs early in p3a_main, before the SD card is mounted, so any mkdir on
    // /sdcard/p3a/pinned/ would fail with ENOENT. The pin-lists module is
    // initialized later from p3a_main, after app_lcd_init (which mounts the
    // SD card and runs sd_path_ensure_directories).

    return ESP_OK;
}
