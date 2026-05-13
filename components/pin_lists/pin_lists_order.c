// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_order.c
 * @brief Atomic load/save and random-access reads of order.bin
 *
 * order.bin layout:
 *   pinned_order_header_t      (16 B: magic, version, count, crc32)
 *   pinned_order_entry_t[count] (64 B each, newest-first)
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

static const char *TAG = "pl_order";

#define ENTRY_SIZE  ((size_t)sizeof(pinned_order_entry_t))

/* ------------------------------------------------------------------------- */
/*  Internal helpers                                                         */
/* ------------------------------------------------------------------------- */

static esp_err_t open_for_read(const char *slug, char *path, size_t path_len, FILE **out_f)
{
    esp_err_t err = pl_paths_order(slug, path, path_len);
    if (err != ESP_OK) return err;
    FILE *f = fopen(path, "rb");
    if (!f) {
        char bak[260];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        f = fopen(bak, "rb");
        if (!f) return ESP_ERR_NOT_FOUND;
        ESP_LOGW(TAG, "%s opened via .bak", path);
    }
    *out_f = f;
    return ESP_OK;
}

static esp_err_t read_header(FILE *f, pinned_order_header_t *hdr)
{
    if (fseek(f, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fread(hdr, 1, sizeof(*hdr), f) != sizeof(*hdr)) return ESP_ERR_INVALID_SIZE;
    if (hdr->magic != PINNED_ORDER_MAGIC) {
        ESP_LOGW(TAG, "order.bin: bad magic 0x%08lx", (unsigned long)hdr->magic);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Public                                                                   */
/* ------------------------------------------------------------------------- */

esp_err_t pl_order_read_header(const char *slug, pinned_order_header_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    char path[256];
    FILE *f;
    esp_err_t err = open_for_read(slug, path, sizeof(path), &f);
    if (err != ESP_OK) return err;
    err = read_header(f, out);
    fclose(f);
    return err;
}

esp_err_t pl_order_read_range(const char *slug, size_t offset, size_t limit,
                              pinned_order_entry_t *out, size_t *out_n)
{
    if (!out || !out_n) return ESP_ERR_INVALID_ARG;
    *out_n = 0;
    if (limit == 0) return ESP_OK;

    char path[256];
    FILE *f;
    esp_err_t err = open_for_read(slug, path, sizeof(path), &f);
    if (err != ESP_OK) return err;

    pinned_order_header_t hdr;
    err = read_header(f, &hdr);
    if (err != ESP_OK) { fclose(f); return err; }

    if (offset >= hdr.count) { fclose(f); return ESP_OK; }
    size_t remaining = hdr.count - offset;
    size_t take = (limit < remaining) ? limit : remaining;

    long byte_offset = (long)sizeof(hdr) + (long)(offset * ENTRY_SIZE);
    if (fseek(f, byte_offset, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t got = fread(out, 1, take * ENTRY_SIZE, f);
    fclose(f);
    if (got != take * ENTRY_SIZE) {
        ESP_LOGW(TAG, "%s: short read at offset=%zu take=%zu got=%zu",
                 path, offset, take, got);
        return ESP_ERR_INVALID_SIZE;
    }
    *out_n = take;
    return ESP_OK;
}

esp_err_t pl_order_read_all(const char *slug, pinned_order_entry_t **out_entries,
                            size_t *out_n)
{
    if (!out_entries || !out_n) return ESP_ERR_INVALID_ARG;
    *out_entries = NULL;
    *out_n = 0;

    char path[256];
    FILE *f;
    esp_err_t err = open_for_read(slug, path, sizeof(path), &f);
    if (err != ESP_OK) return err;

    pinned_order_header_t hdr;
    err = read_header(f, &hdr);
    if (err != ESP_OK) { fclose(f); return err; }

    if (hdr.count == 0) { fclose(f); return ESP_OK; }
    if (hdr.count > PIN_LIST_MAX_ENTRIES) {
        ESP_LOGW(TAG, "%s: count %lu exceeds max", path, (unsigned long)hdr.count);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t bytes = (size_t)hdr.count * ENTRY_SIZE;
    pinned_order_entry_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(bytes);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t got = fread(buf, 1, bytes, f);
    fclose(f);
    if (got != bytes) {
        ESP_LOGW(TAG, "%s: short read all (got %zu of %zu)", path, got, bytes);
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t expected = pl_crc32((const uint8_t *)buf, bytes);
    if (expected != hdr.crc32) {
        ESP_LOGW(TAG, "%s: bad CRC (expected 0x%08lx, got 0x%08lx)",
                 path, (unsigned long)expected, (unsigned long)hdr.crc32);
        free(buf);
        return ESP_ERR_INVALID_CRC;
    }

    *out_entries = buf;
    *out_n = hdr.count;
    return ESP_OK;
}

esp_err_t pl_order_replace(const char *slug, const pinned_order_entry_t *entries, size_t n)
{
    if (n > PIN_LIST_MAX_ENTRIES) return ESP_ERR_INVALID_SIZE;
    if (n > 0 && !entries) return ESP_ERR_INVALID_ARG;

    char path[256];
    esp_err_t err = pl_paths_order(slug, path, sizeof(path));
    if (err != ESP_OK) return err;
    err = sd_path_ensure_parent_dirs(path);
    if (err != ESP_OK) return err;

    char tmp[264];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char bak[264];
    snprintf(bak, sizeof(bak), "%s.bak", path);

    pinned_order_header_t hdr = {
        .magic = PINNED_ORDER_MAGIC,
        .version = PINNED_FORMAT_VERSION,
        .count = (uint32_t)n,
        .crc32 = (n > 0) ? pl_crc32((const uint8_t *)entries, n * ENTRY_SIZE) : 0,
    };

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    bool ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    if (ok && n > 0) {
        ok = (fwrite(entries, 1, n * ENTRY_SIZE, f) == n * ENTRY_SIZE);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "write %s failed", tmp);
        unlink(tmp);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        unlink(bak);
        if (rename(path, bak) != 0) {
            ESP_LOGW(TAG, "rotate %s->%s failed: %s", path, bak, strerror(errno));
        }
    }
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename %s->%s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}
