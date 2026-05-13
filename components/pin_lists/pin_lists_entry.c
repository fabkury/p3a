// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_entry.c
 * @brief Per-pin rich-metadata files at lists/{slug}/entries/{source}_{hash}.bin
 *
 * Each file is exactly sizeof(pinned_entry_file_t) bytes. We write fresh
 * (no atomic temp+rename) because order.bin is the authority — a torn write
 * here just means the next `pl_entry_read` returns ESP_ERR_INVALID_CRC and
 * the row is treated as missing for rich-metadata purposes, while the pin
 * itself remains valid via order.bin.
 */

#include "pin_lists_internal.h"
#include "sd_path.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "pl_entry";

esp_err_t pl_entry_write(const char *slug, const pinned_entry_file_t *e)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    char path[256];
    esp_err_t err = pl_paths_entry(slug, (pinned_source_t)e->source, e->source_id,
                                   path, sizeof(path));
    if (err != ESP_OK) return err;
    err = sd_path_ensure_parent_dirs(path);
    if (err != ESP_OK) return err;

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t want = sizeof(*e);
    bool ok = (fwrite(e, 1, want, f) == want);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "write %s failed", path);
        unlink(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t pl_entry_read(const char *slug, pinned_source_t src, const char *source_id,
                        pinned_entry_file_t *out)
{
    if (!out || !source_id) return ESP_ERR_INVALID_ARG;
    char path[256];
    esp_err_t err = pl_paths_entry(slug, src, source_id, path, sizeof(path));
    if (err != ESP_OK) return err;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t got = fread(out, 1, sizeof(*out), f);
    fclose(f);
    if (got != sizeof(*out)) {
        ESP_LOGW(TAG, "%s: short read (%zu)", path, got);
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->magic != PINNED_ENTRY_MAGIC) {
        ESP_LOGW(TAG, "%s: bad magic 0x%08lx", path, (unsigned long)out->magic);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

bool pl_entry_exists(const char *slug, pinned_source_t src, const char *source_id)
{
    if (!source_id) return false;
    char path[256];
    if (pl_paths_entry(slug, src, source_id, path, sizeof(path)) != ESP_OK) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

esp_err_t pl_entry_delete(const char *slug, pinned_source_t src, const char *source_id)
{
    if (!source_id) return ESP_ERR_INVALID_ARG;
    char path[256];
    esp_err_t err = pl_paths_entry(slug, src, source_id, path, sizeof(path));
    if (err != ESP_OK) return err;
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}
