// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file giphy_cache.c
 * @brief Giphy cache helpers: path building, post_id mapping, channel detection
 */

#include "giphy.h"
#include "giphy_types.h"
#include "sd_path.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "giphy_cache";

// Extension strings matching the extension index (0=webp, 1=gif)
static const char *s_giphy_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

// ============================================================================
// post_id Mapping
// ============================================================================

int32_t giphy_id_to_post_id(const char *giphy_id)
{
    if (!giphy_id || giphy_id[0] == '\0') return -1;

    uint32_t hash = GIPHY_DJB2_SALT;
    unsigned char c;
    while ((c = (unsigned char)*giphy_id++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }

    int32_t post_id = -(int32_t)(hash & 0x7FFFFFFF);
    if (post_id == 0) post_id = -1;  // Avoid 0 (reserved for "no post_id")
    return post_id;
}

// ============================================================================
// Channel Detection
// ============================================================================

bool giphy_is_giphy_channel(const char *channel_id)
{
    if (!channel_id) return false;
    return (strncmp(channel_id, "giphy_", 6) == 0);
}

// ============================================================================
// Path Building
// ============================================================================

esp_err_t giphy_build_filepath(const char *giphy_id, uint8_t extension,
                               char *out_path, size_t out_len)
{
    if (!giphy_id || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get giphy base path
    char giphy_base[128];
    if (sd_path_get_giphy(giphy_base, sizeof(giphy_base)) != ESP_OK) {
        strlcpy(giphy_base, "/sdcard/p3a/giphy", sizeof(giphy_base));
    }

    // Compute SHA256 of giphy_id for sharding
    uint8_t sha256[32];
    int ret = mbedtls_sha256((const unsigned char *)giphy_id, strlen(giphy_id), sha256, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 failed for giphy_id '%s'", giphy_id);
        // Fallback: no sharding
        int ext_idx = (extension <= 1) ? extension : 0;
        snprintf(out_path, out_len, "%s/%s%s", giphy_base, giphy_id, s_giphy_ext_strings[ext_idx]);
        return ESP_OK;
    }

    int ext_idx = (extension <= 1) ? extension : 0;
    snprintf(out_path, out_len, "%s/%02x/%02x/%02x/%s%s",
             giphy_base,
             (unsigned int)sha256[0],
             (unsigned int)sha256[1],
             (unsigned int)sha256[2],
             giphy_id,
             s_giphy_ext_strings[ext_idx]);

    return ESP_OK;
}

void giphy_build_entry_filepath(const giphy_channel_entry_t *entry,
                                char *out_path, size_t out_len)
{
    if (!entry || !out_path || out_len == 0) {
        if (out_path && out_len > 0) out_path[0] = '\0';
        return;
    }

    giphy_build_filepath(entry->giphy_id, entry->extension, out_path, out_len);
}

// ============================================================================
// Initialization
// ============================================================================

static bool s_initialized = false;

esp_err_t giphy_init(void)
{
    if (s_initialized) return ESP_OK;
    ESP_LOGI(TAG, "Giphy component initialized");
    s_initialized = true;
    return ESP_OK;
}

void giphy_deinit(void)
{
    s_initialized = false;
}
