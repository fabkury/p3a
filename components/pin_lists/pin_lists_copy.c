// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_copy.c
 * @brief Local file copy with chunked I/O and atomic temp+rename
 */

#include "pin_lists_internal.h"
#include "sd_path.h"
#include "fs_atomic.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static const char *TAG = "pl_copy";

#define COPY_CHUNK_SIZE  (32 * 1024)

typedef struct {
    int src_fd;
    const char *src_path;
    uint8_t *chunk;
    size_t total;
} copy_writer_ctx_t;

static esp_err_t copy_writer(FILE *dst, void *arg)
{
    copy_writer_ctx_t *ctx = (copy_writer_ctx_t *)arg;

    // read() rather than fread(): newlib's stdio feeds FATFS one sector per SD
    // command (jitter work stream fix 6); read() gets whole-cluster transfers.
    while (true) {
        ssize_t got = read(ctx->src_fd, ctx->chunk, COPY_CHUNK_SIZE);
        if (got < 0) {
            ESP_LOGE(TAG, "read %s error (errno=%d)", ctx->src_path, errno);
            return ESP_FAIL;
        }
        if (got == 0) break;
        if (fwrite(ctx->chunk, 1, (size_t)got, dst) != (size_t)got) {
            ESP_LOGE(TAG, "write short while copying %s", ctx->src_path);
            return ESP_FAIL;
        }
        ctx->total += (size_t)got;
    }
    return ESP_OK;
}

esp_err_t pl_artwork_copy(const char *src_path, const char *dest_path)
{
    if (!src_path || !dest_path || src_path[0] == '\0' || dest_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = sd_path_ensure_parent_dirs(dest_path);
    if (err != ESP_OK) return err;

    int src = open(src_path, O_RDONLY);
    if (src < 0) {
        ESP_LOGE(TAG, "open src %s: %s", src_path, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    // 128-byte aligned so the P4 SD host DMAs straight to/from PSRAM (see psram_alloc.h)
    uint8_t *chunk = heap_caps_aligned_alloc(128, COPY_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) chunk = malloc(COPY_CHUNK_SIZE);
    if (!chunk) {
        close(src);
        return ESP_ERR_NO_MEM;
    }

    /* Atomic copy via the shared helper (tmp + fsync + rename) */
    copy_writer_ctx_t ctx = { .src_fd = src, .src_path = src_path,
                              .chunk = chunk, .total = 0 };
    err = fs_atomic_write_cb(dest_path, copy_writer, &ctx, NULL);
    free(chunk);
    close(src);

    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGD(TAG, "copied %s -> %s (%zu bytes)", src_path, dest_path, ctx.total);
    return ESP_OK;
}
