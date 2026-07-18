// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file fs_atomic.c
 * @brief Shared atomic file-write helper. See fs_atomic.h for semantics.
 */

#include "fs_atomic.h"
#include "sd_health.h"
#include "esp_log.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "fs_atomic";

// Largest destination path seen among migrated call sites is 516 bytes
// (http_api_upload); keep headroom for the ".tmp"/".bak" suffixes.
#define FS_ATOMIC_PATH_MAX 520

#define SD_MOUNT_PREFIX "/sdcard"

static bool is_sd_path(const char *path)
{
    return path && strncmp(path, SD_MOUNT_PREFIX, strlen(SD_MOUNT_PREFIX)) == 0;
}

// Fail-fast when the SD-failure latch has tripped: don't touch a dead card.
static bool refuse_latched(const char *final_path)
{
    if (sd_health_is_failed() && is_sd_path(final_path)) {
        ESP_LOGW(TAG, "SD failure latched, refusing write: %s", final_path);
        return true;
    }
    return false;
}

/**
 * Move tmp_path into place at final_path (both already closed/synced).
 * Default: unlink(final) then rename (FATFS rename won't overwrite).
 * use_bak: rotate final -> "{final}.bak" first, restore it if the rename into
 * place fails, delete it on success.
 * Reports the outcome to sd_health.
 */
static esp_err_t finalize_rename(const char *tmp_path, const char *final_path,
                                 const fs_atomic_opts_t *opts)
{
    bool use_bak = opts && opts->use_bak;

    char bak_path[FS_ATOMIC_PATH_MAX];
    bool had_backup = false;

    if (use_bak) {
        int n = snprintf(bak_path, sizeof(bak_path), "%s.bak", final_path);
        if (n < 0 || n >= (int)sizeof(bak_path)) {
            ESP_LOGE(TAG, "Path too long for backup: %s", final_path);
            unlink(tmp_path);
            return ESP_ERR_INVALID_ARG;
        }
        unlink(bak_path);  // clean stale backup

        struct stat st;
        if (stat(final_path, &st) == 0) {
            if (rename(final_path, bak_path) == 0) {
                had_backup = true;
            } else {
                // rename-to-backup failed; fall back to unlink
                ESP_LOGW(TAG, "Backup rename failed (%s -> %s, errno=%d), falling back to unlink",
                         final_path, bak_path, errno);
                unlink(final_path);
            }
        }
    } else {
        unlink(final_path);  // FATFS rename fails if destination exists
    }

    if (rename(tmp_path, final_path) != 0) {
        int rename_errno = errno;
        ESP_LOGE(TAG, "Rename failed: %s -> %s (errno=%d)", tmp_path, final_path, rename_errno);
        unlink(tmp_path);
        if (had_backup) {
            if (rename(bak_path, final_path) != 0) {
                ESP_LOGE(TAG, "Backup restore also failed: %s -> %s (errno=%d)",
                         bak_path, final_path, errno);
            } else {
                ESP_LOGW(TAG, "Restored previous version from backup: %s", final_path);
            }
        }
        sd_health_report_write_failure(final_path, rename_errno);
        return ESP_FAIL;
    }

    if (had_backup) {
        unlink(bak_path);
    }

    sd_health_report_write_ok(final_path);
    return ESP_OK;
}

esp_err_t fs_atomic_write_cb(const char *final_path, fs_atomic_writer_cb_t writer,
                             void *ctx, const fs_atomic_opts_t *opts)
{
    if (!final_path || !writer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (refuse_latched(final_path)) {
        return ESP_ERR_INVALID_STATE;
    }

    char tmp_path[FS_ATOMIC_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    if (n < 0 || n >= (int)sizeof(tmp_path)) {
        ESP_LOGE(TAG, "Path too long: %s", final_path);
        return ESP_ERR_INVALID_ARG;
    }

    unlink(tmp_path);  // clean orphan from a previous interrupted save

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        int open_errno = errno;
        ESP_LOGE(TAG, "Failed to create temp file: %s (errno=%d)", tmp_path, open_errno);
        sd_health_report_write_failure(final_path, open_errno);
        return ESP_FAIL;
    }

    esp_err_t err = writer(f, ctx);
    if (err != ESP_OK) {
        // Writer errors are the caller's domain (may be network-side):
        // clean up, but do not report to sd_health.
        fclose(f);
        unlink(tmp_path);
        return err;
    }

    fflush(f);
    if (fsync(fileno(f)) != 0) {
        int sync_errno = errno;
        ESP_LOGE(TAG, "fsync failed: %s (errno=%d)", tmp_path, sync_errno);
        fclose(f);
        unlink(tmp_path);
        sd_health_report_write_failure(final_path, sync_errno);
        return ESP_FAIL;
    }
    fclose(f);

    return finalize_rename(tmp_path, final_path, opts);
}

typedef struct {
    const void *data;
    size_t len;
    const char *final_path;
} buffer_writer_ctx_t;

static esp_err_t buffer_writer(FILE *f, void *ctx)
{
    buffer_writer_ctx_t *bw = (buffer_writer_ctx_t *)ctx;
    if (bw->len > 0 && fwrite(bw->data, 1, bw->len, f) != bw->len) {
        int write_errno = errno;
        ESP_LOGE(TAG, "Short write of %s (%zu bytes, errno=%d)",
                 bw->final_path, bw->len, write_errno);
        // Buffer writes have no network ambiguity: report directly.
        sd_health_report_write_failure(bw->final_path, write_errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t fs_atomic_write(const char *final_path, const void *data, size_t len,
                          const fs_atomic_opts_t *opts)
{
    if (!final_path || (!data && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer_writer_ctx_t ctx = { .data = data, .len = len, .final_path = final_path };
    return fs_atomic_write_cb(final_path, buffer_writer, &ctx, opts);
}

esp_err_t fs_atomic_rename(const char *tmp_path, const char *final_path,
                           const fs_atomic_opts_t *opts)
{
    if (!tmp_path || !final_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (refuse_latched(final_path)) {
        unlink(tmp_path);  // caller's tmp is orphaned either way; tidy up
        return ESP_ERR_INVALID_STATE;
    }
    return finalize_rename(tmp_path, final_path, opts);
}
