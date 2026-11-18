// SPDX-License-Identifier: Apache-2.0
// The file half of p3a_frame.h: saving and loading containers on the card.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "fs_atomic.h"
#include "p3a_crc32.h"
#include "p3a_err.h"
#include "p3a_frame.h"
#include "psram_alloc.h"
#include "sd_path.h"

static const char *TAG = "p3a_frame";

typedef struct {
    const p3a_frame_header_t *header;
    const void *records;
    size_t records_len;
} save_ctx_t;

static esp_err_t write_frame(FILE *f, void *arg)
{
    const save_ctx_t *ctx = arg;
    if (fwrite(ctx->header, 1, sizeof(*ctx->header), f) != sizeof(*ctx->header)) {
        return ESP_FAIL;
    }
    if (ctx->records_len && fwrite(ctx->records, 1, ctx->records_len, f) != ctx->records_len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t p3a_frame_save(const char *path, uint16_t schema, uint16_t version, uint16_t record_size, uint32_t count,
                         uint32_t flags, const void *records)
{
    if (!path || (count && !records) || record_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sd_path_ensure_parent_dirs(path);
    if (err != ESP_OK) {
        return err;
    }
    p3a_frame_header_t header;
    p3a_frame_header_make(&header, schema, version, record_size, count, flags, records);
    save_ctx_t ctx = {.header = &header, .records = records, .records_len = (size_t)count * record_size};
    // What the user made keeps its previous version until the new one is in place.
    const fs_atomic_opts_t opts = {.use_bak = (flags & P3A_FRAME_USER_DATA) != 0};
    return fs_atomic_write_cb(path, write_frame, &ctx, &opts);
}

esp_err_t p3a_frame_load(const char *path, const p3a_frame_expect_t *expect, void **records, uint32_t *count,
                         uint32_t *flags)
{
    if (!path || !expect || !records || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *records = NULL;
    *count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT ? P3A_ERR_MISSING : ESP_FAIL;
    }

    p3a_frame_header_t header;
    size_t got = fread(&header, 1, sizeof(header), f);
    size_t records_len = 0;
    esp_err_t err = p3a_frame_header_check(&header, got, expect, &records_len);

    void *buffer = NULL;
    if (err == ESP_OK && records_len > 0) {
        buffer = psram_malloc(records_len);
        if (!buffer) {
            err = ESP_ERR_NO_MEM;
        } else if (fread(buffer, 1, records_len, f) != records_len) {
            err = ESP_ERR_INVALID_SIZE;  // truncated
        } else if (p3a_crc32(0, buffer, records_len) != header.records_crc) {
            err = P3A_ERR_CORRUPT;
        }
    }
    fclose(f);

    if (err != ESP_OK) {
        free(buffer);
        // The file stays exactly as it is: see "THE RULE" in p3a_frame.h.
        ESP_LOGW(TAG, "%s failed its check: %s", path, p3a_err_to_name(err));
        return err;
    }
    *records = buffer;
    *count = header.count;
    if (flags) {
        *flags = header.flags;
    }
    return ESP_OK;
}
