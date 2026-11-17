// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy_cache.c
 * @brief Klipy cache helpers: path building, post_id mapping, init
 */

#include "klipy.h"
#include "klipy_types.h"
#include "sd_path.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "klipy_cache";

// Extension strings matching the extension index (0=webp, 1=gif)
static const char *s_klipy_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

// ============================================================================
// post_id Mapping
// ============================================================================

int32_t klipy_id_to_post_id(uint64_t klipy_id)
{
    if (klipy_id == 0) return 0;

    // Fold the 64-bit id into 31 bits; mix the salt for a little extra
    // dispersion so unrelated Klipy ids are unlikely to collide in the cache.
    uint64_t h = klipy_id ^ (klipy_id >> 32);
    h ^= KLIPY_DJB2_SALT;

    int32_t post_id = (int32_t)(h & 0x7FFFFFFF);
    if (post_id == 0) post_id = 1;  // Avoid 0 (reserved for "no post_id")
    return post_id;
}

// ============================================================================
// Path Building
// ============================================================================

static const char *klipy_product_dir(uint8_t product)
{
    return (product == KLIPY_PRODUCT_STICKER) ? "sticker" : "gif";
}

esp_err_t klipy_build_filepath(uint64_t klipy_id, uint8_t product, uint8_t extension,
                               char *out_path, size_t out_len)
{
    if (!out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char klipy_base[128];
    esp_err_t path_err = sd_path_get_klipy(klipy_base, sizeof(klipy_base));
    if (path_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot resolve klipy directory: %s", esp_err_to_name(path_err));
        return path_err;
    }

    // Shard per product so gif/ and sticker/ caches stay separate, mirroring the
    // per-museum sharding under /sdcard/p3a/museum/{museum_id}/.
    char prod_base[160];
    snprintf(prod_base, sizeof(prod_base), "%s/%s", klipy_base, klipy_product_dir(product));

    char id_str[24];
    snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)klipy_id);

    int ext_idx = (extension <= 1) ? extension : 0;
    return sd_path_build_sharded(prod_base, id_str,
                                 s_klipy_ext_strings[ext_idx], out_path, out_len);
}

void klipy_build_entry_filepath(const klipy_channel_entry_t *entry,
                                char *out_path, size_t out_len)
{
    if (!entry || !out_path || out_len == 0) {
        if (out_path && out_len > 0) out_path[0] = '\0';
        return;
    }

    if (klipy_build_filepath(entry->klipy_id, entry->product, entry->extension,
                             out_path, out_len) != ESP_OK) {
        out_path[0] = '\0';
    }
}

// ============================================================================
// Initialization
// ============================================================================

static bool s_initialized = false;

esp_err_t klipy_init(void)
{
    if (s_initialized) return ESP_OK;
    ESP_LOGI(TAG, "Klipy component initialized");
    s_initialized = true;
    return ESP_OK;
}

void klipy_deinit(void)
{
    s_initialized = false;
}
