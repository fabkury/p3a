// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file play_scheduler_cache.c
 * @brief SD card index building for Play Scheduler
 *
 * Scans /sdcard/p3a/animations/ and builds a binary index file at
 * /sdcard/p3a/channel/sdcard.bin using the same format as Makapix channels.
 */

#include "play_scheduler_internal.h"
#include "makapix_channel_impl.h"
#include "sd_path.h"
#include "esp_log.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "ps_cache";

// DJB2 hash for stable post_id generation
static uint32_t hash_djb2(const char *s)
{
    uint32_t hash = 5381u;
    unsigned char c;
    while (s && (c = (unsigned char)*s++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }
    return hash;
}

// Detect asset type from filename extension, returns extension enum (matches makapix_channel_internal.h)
static int detect_extension_from_name(const char *name, bool *out_ok)
{
    if (out_ok) *out_ok = false;
    if (!name) return 0;

    size_t len = strlen(name);

    if (len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) {
        if (out_ok) *out_ok = true;
        return 0; // EXT_WEBP
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) {
        if (out_ok) *out_ok = true;
        return 1; // EXT_GIF
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) {
        if (out_ok) *out_ok = true;
        return 2; // EXT_PNG
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) {
        if (out_ok) *out_ok = true;
        return 3; // EXT_JPEG
    }
    if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) {
        if (out_ok) *out_ok = true;
        return 3; // EXT_JPEG
    }

    return 0;
}

esp_err_t ps_build_sdcard_index(void)
{
    char animations_path[128];
    char channel_path[128];

    esp_err_t err = sd_path_get_animations(animations_path, sizeof(animations_path));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get animations path");
        return err;
    }

    err = sd_path_get_channel(channel_path, sizeof(channel_path));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get channel path");
        return err;
    }

    // Build output path
    char index_path[160];
    snprintf(index_path, sizeof(index_path), "%s/sdcard.bin", channel_path);

    ESP_LOGI(TAG, "Building SD card index from %s", animations_path);

    DIR *dir = opendir(animations_path);
    if (!dir) {
        if (errno == ENOENT) {
            ESP_LOGW(TAG, "Animations directory not found: %s", animations_path);
            // Write empty index
            FILE *f = fopen(index_path, "wb");
            if (f) fclose(f);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to open directory: %s (errno=%d)", animations_path, errno);
        return ESP_FAIL;
    }

    // Pass 1: count eligible files
    struct dirent *entry;
    size_t count = 0;
    const size_t MAX_ENTRIES = 1024;

    while ((entry = readdir(dir)) != NULL && count < MAX_ENTRIES) {
        if (entry->d_name[0] == '.') continue;

        bool type_ok = false;
        (void)detect_extension_from_name(entry->d_name, &type_ok);
        if (type_ok) {
            count++;
        }
    }

    if (count == 0) {
        closedir(dir);
        ESP_LOGI(TAG, "No image files found, creating empty index");
        // Write empty index
        FILE *f = fopen(index_path, "wb");
        if (f) fclose(f);
        return ESP_OK;
    }

    // Allocate entries array
    makapix_channel_entry_t *entries = calloc(count, sizeof(makapix_channel_entry_t));
    if (!entries) {
        closedir(dir);
        ESP_LOGE(TAG, "Failed to allocate %zu entries", count);
        return ESP_ERR_NO_MEM;
    }

    // Pass 2: populate entries
    rewinddir(dir);
    size_t idx = 0;

    while ((entry = readdir(dir)) != NULL && idx < count) {
        if (entry->d_name[0] == '.') continue;

        bool type_ok = false;
        int ext = detect_extension_from_name(entry->d_name, &type_ok);
        if (!type_ok) continue;

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", animations_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) continue;

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        makapix_channel_entry_t *e = &entries[idx];
        memset(e, 0, sizeof(*e));

        // Use negative post_id to avoid collision with server IDs
        // The DJB2 hash is cast to int32_t and made negative
        uint32_t h = hash_djb2(entry->d_name);
        e->post_id = -(int32_t)(h & 0x7FFFFFFF);
        if (e->post_id == 0) e->post_id = -1; // Avoid 0

        e->kind = 0; // MAKAPIX_INDEX_POST_KIND_ARTWORK
        e->extension = (uint8_t)ext;
        e->filter_flags = 0;
        e->created_at = (uint32_t)st.st_mtime;
        e->metadata_modified_at = (uint32_t)st.st_mtime;
        e->artwork_modified_at = (uint32_t)st.st_mtime;
        e->dwell_time_ms = 0; // Use default
        e->total_artworks = 0;
        // storage_key_uuid stays all zeros (local file indicator)

        idx++;
    }

    closedir(dir);

    if (idx == 0) {
        free(entries);
        ESP_LOGI(TAG, "No valid entries after scan");
        FILE *f = fopen(index_path, "wb");
        if (f) fclose(f);
        return ESP_OK;
    }

    // Ensure channel directory exists
    struct stat st;
    if (stat(channel_path, &st) != 0) {
        if (mkdir(channel_path, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create channel directory: %s", channel_path);
            free(entries);
            return ESP_FAIL;
        }
    }

    // Atomic write: write to temp file, then rename
    char temp_path[164];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", index_path);

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open temp file: %s (errno=%d)", temp_path, errno);
        free(entries);
        return ESP_FAIL;
    }

    size_t written = fwrite(entries, sizeof(makapix_channel_entry_t), idx, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(entries);

    if (written != idx) {
        ESP_LOGE(TAG, "Failed to write entries: %zu/%zu", written, idx);
        unlink(temp_path);
        return ESP_FAIL;
    }

    // FATFS doesn't overwrite on rename, so unlink first
    if (unlink(index_path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "Failed to unlink old index: %s", index_path);
    }

    if (rename(temp_path, index_path) != 0) {
        int e = errno;
        ESP_LOGE(TAG, "Failed to rename temp to index: errno=%d", e);
        unlink(temp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD card index built: %zu entries", idx);
    return ESP_OK;
}

esp_err_t ps_touch_cache_file(const char *channel_id)
{
    if (!channel_id) return ESP_ERR_INVALID_ARG;

    char channel_path[128];
    esp_err_t err = sd_path_get_channel(channel_path, sizeof(channel_path));
    if (err != ESP_OK) return err;

    char path[192];
    snprintf(path, sizeof(path), "%s/%s.bin", channel_path, channel_id);

    // Read current times, then set both to now
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // Touch file by opening for append and closing
    // This updates mtime on most filesystems
    FILE *f = fopen(path, "ab");
    if (f) {
        fclose(f);
    }

    return ESP_OK;
}
