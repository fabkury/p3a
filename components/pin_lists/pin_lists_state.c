// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_state.c
 * @brief Atomic load/save of {sd-root}/pinned/state.bin (active list slug)
 */

#include "pin_lists_internal.h"
#include "sd_path.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "pl_state";

/* CRC covers all bytes BEFORE the crc32 field: magic + version + active_slug. */
#define CRC_PAYLOAD_BYTES  (offsetof(pinned_state_t, crc32))

static esp_err_t read_state_file(const char *path, pinned_state_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(out, 1, sizeof(*out), f);
    fclose(f);
    if (n != sizeof(*out)) {
        ESP_LOGW(TAG, "%s: short read (%zu of %zu)", path, n, sizeof(*out));
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->magic != PINNED_STATE_MAGIC) {
        ESP_LOGW(TAG, "%s: bad magic 0x%08lx", path, (unsigned long)out->magic);
        return ESP_ERR_INVALID_CRC;
    }
    uint32_t expected = pl_crc32((const uint8_t *)out, CRC_PAYLOAD_BYTES);
    if (expected != out->crc32) {
        ESP_LOGW(TAG, "%s: bad CRC (expected 0x%08lx, got 0x%08lx)",
                 path, (unsigned long)expected, (unsigned long)out->crc32);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t pl_state_load(pinned_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    char path[200];
    esp_err_t err = pl_paths_state(path, sizeof(path));
    if (err != ESP_OK) return err;

    err = read_state_file(path, out);
    if (err == ESP_OK) {
        if (out->version > PINNED_FORMAT_VERSION) {
            ESP_LOGW(TAG, "state.bin version %lu > %u; treating as read-only",
                     (unsigned long)out->version, PINNED_FORMAT_VERSION);
        }
        return ESP_OK;
    }

    /* Try the backup. */
    char bak[208];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    esp_err_t e2 = read_state_file(bak, out);
    if (e2 == ESP_OK) {
        ESP_LOGW(TAG, "state.bin recovered from .bak");
        return ESP_OK;
    }
    return err;  /* original error */
}

esp_err_t pl_state_save(const pinned_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;

    char path[200];
    esp_err_t err = pl_paths_state(path, sizeof(path));
    if (err != ESP_OK) return err;
    err = sd_path_ensure_parent_dirs(path);
    if (err != ESP_OK) return err;

    /* Build the to-be-written record (fill magic/version/CRC). */
    pinned_state_t out = *state;
    out.magic = PINNED_STATE_MAGIC;
    out.version = PINNED_FORMAT_VERSION;
    memset(out.reserved, 0, sizeof(out.reserved));
    out.crc32 = pl_crc32((const uint8_t *)&out, CRC_PAYLOAD_BYTES);

    char tmp[208];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char bak[208];
    snprintf(bak, sizeof(bak), "%s.bak", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    if (fwrite(&out, 1, sizeof(out), f) != sizeof(out)) {
        ESP_LOGE(TAG, "write %s failed", tmp);
        fclose(f);
        unlink(tmp);
        return ESP_FAIL;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* Rotate prior primary to .bak (best-effort; ignore if no primary yet). */
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
