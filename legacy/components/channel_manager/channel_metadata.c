// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file channel_metadata.c
 * @brief Generic per-channel metadata persistence (JSON sidecar files)
 *
 * Provides save/load for channel metadata stored alongside the binary
 * .cache files. Used by both Makapix and Giphy channels.
 */

#include "channel_metadata.h"
#include "fs_atomic.h"
#include "psram_alloc.h"
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

    // Atomic write with .bak rotation: the previous metadata survives (and is
    // restored) if the rename into place fails.
    fs_atomic_opts_t opts = { .use_bak = true };
    esp_err_t err = fs_atomic_write(meta_path, json_str, strlen(json_str), &opts);
    free(json_str);
    return err;
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

    // Note: orphan .tmp files are cleaned up by the save function (line 59)
    // before each write. Do NOT clean them up here — concurrent loads would
    // race with an in-progress save and delete its temp file mid-rename.

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

    char *json_buf = psram_malloc(size + 1);
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
