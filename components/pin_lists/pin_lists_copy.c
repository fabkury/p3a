// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_copy.c
 * @brief Local file copy with chunked I/O and atomic temp+rename
 */

#include "pin_lists_internal.h"
#include "sd_path.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "pl_copy";

#define COPY_CHUNK_SIZE  (32 * 1024)

esp_err_t pl_artwork_copy(const char *src_path, const char *dest_path)
{
    if (!src_path || !dest_path || src_path[0] == '\0' || dest_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = sd_path_ensure_parent_dirs(dest_path);
    if (err != ESP_OK) return err;

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        ESP_LOGE(TAG, "open src %s: %s", src_path, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    char tmp[264];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest_path);
    FILE *dst = fopen(tmp, "wb");
    if (!dst) {
        ESP_LOGE(TAG, "open tmp %s: %s", tmp, strerror(errno));
        fclose(src);
        return ESP_FAIL;
    }

    uint8_t *chunk = heap_caps_malloc(COPY_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) chunk = malloc(COPY_CHUNK_SIZE);
    if (!chunk) {
        fclose(src);
        fclose(dst);
        unlink(tmp);
        return ESP_ERR_NO_MEM;
    }

    bool ok = true;
    size_t total = 0;
    while (ok) {
        size_t got = fread(chunk, 1, COPY_CHUNK_SIZE, src);
        if (got == 0) break;
        if (fwrite(chunk, 1, got, dst) != got) {
            ESP_LOGE(TAG, "write %s short", tmp);
            ok = false;
            break;
        }
        total += got;
    }
    if (ferror(src)) {
        ESP_LOGE(TAG, "read %s error", src_path);
        ok = false;
    }
    free(chunk);
    fclose(src);
    fflush(dst);
    fsync(fileno(dst));
    fclose(dst);

    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }

    unlink(dest_path);  /* in case a stale file exists */
    if (rename(tmp, dest_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tmp, dest_path, strerror(errno));
        unlink(tmp);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "copied %s -> %s (%zu bytes)", src_path, dest_path, total);
    return ESP_OK;
}
