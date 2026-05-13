// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists_manifest.c
 * @brief Atomic JSON load/save for per-list manifest.json
 */

#include "pin_lists_internal.h"
#include "sd_path.h"
#include "cJSON.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "pl_manifest";

static esp_err_t read_manifest_file(const char *path, pl_manifest_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if ((long)got != sz) {
        free(buf);
        return ESP_FAIL;
    }
    buf[sz] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, sz);
    free(buf);
    if (!root) return ESP_ERR_INVALID_CRC;  /* using INVALID_CRC to mean "corrupt" */

    memset(out, 0, sizeof(*out));
    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "version")) && cJSON_IsNumber(j)) {
        out->version = (uint32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(root, "slug")) && cJSON_IsString(j) && j->valuestring) {
        strlcpy(out->slug, j->valuestring, sizeof(out->slug));
    }
    if ((j = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(j) && j->valuestring) {
        strlcpy(out->name, j->valuestring, sizeof(out->name));
    }
    if ((j = cJSON_GetObjectItem(root, "created_at")) && cJSON_IsNumber(j)) {
        out->created_at = (uint32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(root, "next_post_id")) && cJSON_IsNumber(j)) {
        out->next_post_id = (int32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItem(root, "count_cache")) && cJSON_IsNumber(j)) {
        out->count_cache = (uint32_t)j->valuedouble;
    }
    cJSON_Delete(root);

    /* Minimal sanity. */
    if (out->version == 0) {
        ESP_LOGW(TAG, "%s: missing version", path);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!pl_slug_is_valid(out->slug)) {
        ESP_LOGW(TAG, "%s: invalid slug '%s'", path, out->slug);
        return ESP_ERR_INVALID_ARG;
    }
    if (out->next_post_id < 1) {
        out->next_post_id = 1;
    }
    return ESP_OK;
}

esp_err_t pl_manifest_load(const char *slug, pl_manifest_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    char path[220];
    esp_err_t err = pl_paths_manifest(slug, path, sizeof(path));
    if (err != ESP_OK) return err;

    err = read_manifest_file(path, out);
    if (err == ESP_OK) {
        if (out->version > PINNED_FORMAT_VERSION) {
            ESP_LOGW(TAG, "%s: version %lu > %u; read-only",
                     path, (unsigned long)out->version, PINNED_FORMAT_VERSION);
        }
        return ESP_OK;
    }

    /* Try the .bak. */
    char bak[228];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    esp_err_t e2 = read_manifest_file(bak, out);
    if (e2 == ESP_OK) {
        ESP_LOGW(TAG, "%s recovered from .bak", path);
        return ESP_OK;
    }
    return err;
}

esp_err_t pl_manifest_save(const char *slug, const pl_manifest_t *m)
{
    if (!m) return ESP_ERR_INVALID_ARG;
    char path[220];
    esp_err_t err = pl_paths_manifest(slug, path, sizeof(path));
    if (err != ESP_OK) return err;
    err = sd_path_ensure_parent_dirs(path);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "version", PINNED_FORMAT_VERSION);
    cJSON_AddStringToObject(root, "slug", m->slug);
    cJSON_AddStringToObject(root, "name", m->name);
    cJSON_AddNumberToObject(root, "created_at", m->created_at);
    cJSON_AddNumberToObject(root, "next_post_id", m->next_post_id);
    cJSON_AddNumberToObject(root, "count_cache", m->count_cache);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;
    size_t len = strlen(str);

    char tmp[228];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char bak[228];
    snprintf(bak, sizeof(bak), "%s.bak", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", tmp, strerror(errno));
        free(str);
        return ESP_FAIL;
    }
    bool ok = (fwrite(str, 1, len, f) == len);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(str);
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
