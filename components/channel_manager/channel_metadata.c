// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file channel_metadata.c
 * @brief Generic per-channel metadata persistence (JSON sidecar files)
 *
 * Provides save/load for channel metadata stored alongside the binary
 * .cache files. Used by both Makapix and Giphy channels.
 */

#include "channel_metadata.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "ch_metadata";

esp_err_t channel_metadata_save(const char *channel_id,
                                const char *channels_path,
                                const channel_metadata_t *meta)
{
    if (!channel_id || !channels_path || !meta) {
        return ESP_ERR_INVALID_ARG;
    }

    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", channels_path, channel_id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    if (meta->cursor[0] != '\0') {
        cJSON_AddStringToObject(root, "cursor", meta->cursor);
    } else {
        cJSON_AddNullToObject(root, "cursor");
    }
    cJSON_AddNumberToObject(root, "last_refresh", (double)meta->last_refresh);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    // Atomic write: write to temp file, then rename
    char temp_path[260];
    size_t path_len = strlen(meta_path);
    if (path_len + 4 >= sizeof(temp_path)) {
        ESP_LOGE(TAG, "Meta path too long for temp file");
        free(json_str);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", meta_path);

    // Clean up any orphan temp file from previous interrupted save
    unlink(temp_path);

    FILE *f = fopen(temp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create temp file: %s (errno=%d)", temp_path, errno);
        free(json_str);
        return ESP_FAIL;
    }

    fprintf(f, "%s", json_str);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(json_str);

    // On FAT filesystems (SD card), rename() fails if destination exists.
    // Delete the destination first, then rename.
    struct stat st;
    if (stat(meta_path, &st) == 0) {
        if (unlink(meta_path) != 0) {
            ESP_LOGW(TAG, "Failed to remove old metadata file: %s (errno=%d)", meta_path, errno);
        }
    }

    if (rename(temp_path, meta_path) != 0) {
        ESP_LOGE(TAG, "Rename failed: %s -> %s (errno=%d)", temp_path, meta_path, errno);
        unlink(temp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t channel_metadata_load(const char *channel_id,
                                const char *channels_path,
                                channel_metadata_t *out_meta)
{
    if (!channel_id || !channels_path || !out_meta) {
        return ESP_ERR_INVALID_ARG;
    }

    // Zero output first
    memset(out_meta, 0, sizeof(*out_meta));

    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", channels_path, channel_id);

    // Clean up orphan .tmp file if it exists (lazy cleanup)
    char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", meta_path);
    struct stat tmp_st;
    if (stat(tmp_path, &tmp_st) == 0 && S_ISREG(tmp_st.st_mode)) {
        ESP_LOGD(TAG, "Removing orphan temp file: %s", tmp_path);
        unlink(tmp_path);
    }

    FILE *f = fopen(meta_path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4096) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *json_buf = malloc(size + 1);
    if (!json_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    fread(json_buf, 1, size, f);
    json_buf[size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *cursor_item = cJSON_GetObjectItem(root, "cursor");
    if (cJSON_IsString(cursor_item)) {
        strncpy(out_meta->cursor, cursor_item->valuestring, sizeof(out_meta->cursor) - 1);
        out_meta->cursor[sizeof(out_meta->cursor) - 1] = '\0';
    }

    cJSON *refresh_item = cJSON_GetObjectItem(root, "last_refresh");
    if (cJSON_IsNumber(refresh_item)) {
        out_meta->last_refresh = (time_t)cJSON_GetNumberValue(refresh_item);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
