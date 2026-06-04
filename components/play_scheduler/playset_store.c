// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playset_store.c
 * @brief Playset storage implementation
 *
 * Uses hash-based filenames (ps_{djb2}.playset) to decouple playset names
 * from filesystem constraints. The playset name is stored inside the file
 * header.
 */

#include "playset_store.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

static const char *TAG = "playset_store";

static uint32_t djb2_hash(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/**
 * Resolve the channel directory under the configured SD root at runtime.
 * Playsets deliberately live under the user-configurable root so a root
 * switch behaves as a cold start (no cross-root playset bleed-through).
 */
static esp_err_t get_playset_dir(char *out_dir, size_t dir_len)
{
    esp_err_t err = sd_path_get_channel(out_dir, dir_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resolve channel directory: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t build_path(const char *name, char *out_path, size_t path_len)
{
    char dir[128];
    esp_err_t err = get_playset_dir(dir, sizeof(dir));
    if (err != ESP_OK) return err;

    int written = snprintf(out_path, path_len, "%s/ps_%08lx.playset",
                           dir, (unsigned long)djb2_hash(name));
    if (written < 0 || (size_t)written >= path_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static esp_err_t build_tmp_path(const char *name, char *out_path, size_t path_len)
{
    char dir[128];
    esp_err_t err = get_playset_dir(dir, sizeof(dir));
    if (err != ESP_OK) return err;

    int written = snprintf(out_path, path_len, "%s/ps_%08lx.playset.tmp",
                           dir, (unsigned long)djb2_hash(name));
    if (written < 0 || (size_t)written >= path_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static uint32_t calculate_checksum(const playset_header_t *header,
                                    const playset_channel_entry_t *entries,
                                    size_t entry_count)
{
    playset_header_t hdr_copy = *header;
    hdr_copy.checksum = 0;

    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&hdr_copy, sizeof(hdr_copy));

    if (entries && entry_count > 0) {
        crc = esp_rom_crc32_le(crc, (const uint8_t *)entries,
                               entry_count * sizeof(playset_channel_entry_t));
    }

    return crc;
}

esp_err_t playset_store_save(const char *name, const ps_playset_t *playset)
{
    if (!name || !playset || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (playset->channel_count == 0 || playset->channel_count > PS_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel count: %zu", playset->channel_count);
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    char tmp_path[128];
    esp_err_t err = build_path(name, path, sizeof(path));
    if (err == ESP_OK) {
        err = build_tmp_path(name, tmp_path, sizeof(tmp_path));
    }
    if (err != ESP_OK) {
        return err;
    }

    // Boot creates {root}/channel (sd_path_ensure_directories), but recreate
    // on demand in case the directory disappeared since boot.
    err = sd_path_ensure_parent_dirs(path);
    if (err != ESP_OK) {
        return err;
    }

    playset_header_t header = {0};
    header.magic = PLAYSET_MAGIC;
    header.version = PLAYSET_VERSION;
    header.flags = 0;
    header._reserved_exposure_mode = 0;  // Legacy field; preserved for binary compat with v11
    header._reserved_pick_mode = 0;      // Legacy field; pick_mode is now global in config_store
    header.channel_count = (uint16_t)playset->channel_count;
    strlcpy(header.name, name, sizeof(header.name));

    playset_channel_entry_t *entries = calloc(playset->channel_count, sizeof(playset_channel_entry_t));
    if (!entries) {
        ESP_LOGE(TAG, "Failed to allocate entries");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < playset->channel_count; i++) {
        const ps_channel_spec_t *src = &playset->channels[i];
        playset_channel_entry_t *dst = &entries[i];

        dst->type = (uint8_t)src->type;
        strncpy(dst->name, src->name, sizeof(dst->name) - 1);
        strncpy(dst->identifier, src->identifier, sizeof(dst->identifier) - 1);
        strncpy(dst->display_name, src->display_name, sizeof(dst->display_name) - 1);
        dst->weight = src->weight;
        dst->offset = src->offset;
    }

    header.checksum = calculate_checksum(&header, entries, playset->channel_count);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s", tmp_path, strerror(errno));
        free(entries);
        return ESP_FAIL;
    }

    bool write_ok = true;

    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to write header");
        write_ok = false;
    }

    if (write_ok && fwrite(entries, sizeof(playset_channel_entry_t), playset->channel_count, f) != playset->channel_count) {
        ESP_LOGE(TAG, "Failed to write entries");
        write_ok = false;
    }

    if (write_ok) {
        fflush(f);
        fsync(fileno(f));
    }

    fclose(f);
    free(entries);

    if (!write_ok) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    unlink(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved playset '%s' with %zu channels", name, playset->channel_count);
    return ESP_OK;
}

esp_err_t playset_store_load(const char *name, ps_playset_t *out_playset)
{
    if (!name || !out_playset || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    esp_err_t path_err = build_path(name, path, sizeof(path));
    if (path_err != ESP_OK) {
        return path_err;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    playset_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to read header");
        fclose(f);
        return ESP_FAIL;
    }

    if (header.magic != PLAYSET_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08lX (expected 0x%08X)",
                 (unsigned long)header.magic, PLAYSET_MAGIC);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    if (header.version != PLAYSET_VERSION) {
        ESP_LOGW(TAG, "Version mismatch: %u (expected %u), deleting file",
                 header.version, PLAYSET_VERSION);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_VERSION;
    }

    if (header.channel_count == 0 || header.channel_count > PS_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel count: %u", header.channel_count);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    // Guard against hash collisions: verify stored name matches requested name
    header.name[sizeof(header.name) - 1] = '\0';
    if (strcmp(header.name, name) != 0) {
        ESP_LOGW(TAG, "Hash collision: file contains '%s', requested '%s'", header.name, name);
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    playset_channel_entry_t *entries = calloc(header.channel_count, sizeof(playset_channel_entry_t));
    if (!entries) {
        ESP_LOGE(TAG, "Failed to allocate entries");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (fread(entries, sizeof(playset_channel_entry_t), header.channel_count, f) != header.channel_count) {
        ESP_LOGE(TAG, "Failed to read entries");
        free(entries);
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);

    uint32_t expected_crc = header.checksum;
    uint32_t actual_crc = calculate_checksum(&header, entries, header.channel_count);
    if (actual_crc != expected_crc) {
        ESP_LOGE(TAG, "CRC mismatch: 0x%08lX (expected 0x%08lX)",
                 (unsigned long)actual_crc, (unsigned long)expected_crc);
        free(entries);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    memset(out_playset, 0, sizeof(*out_playset));
    strlcpy(out_playset->name, header.name, sizeof(out_playset->name));
    out_playset->channel_count = header.channel_count;

    for (size_t i = 0; i < header.channel_count; i++) {
        const playset_channel_entry_t *src = &entries[i];
        ps_channel_spec_t *dst = &out_playset->channels[i];

        dst->type = (ps_channel_type_t)src->type;
        strncpy(dst->name, src->name, sizeof(dst->name) - 1);
        strncpy(dst->identifier, src->identifier, sizeof(dst->identifier) - 1);
        strncpy(dst->display_name, src->display_name, sizeof(dst->display_name) - 1);
        dst->weight = src->weight;
        dst->offset = src->offset;
    }

    free(entries);

    ESP_LOGI(TAG, "Loaded playset '%s' with %zu channels", name, out_playset->channel_count);
    return ESP_OK;
}

bool playset_store_exists(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return false;
    }

    char path[128];
    if (build_path(name, path, sizeof(path)) != ESP_OK) {
        return false;
    }

    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t playset_store_delete(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    esp_err_t path_err = build_path(name, path, sizeof(path));
    if (path_err != ESP_OK) {
        return path_err;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "Failed to delete %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted playset '%s'", name);
    return ESP_OK;
}

esp_err_t playset_store_list(playset_list_entry_t *out, size_t max, size_t *out_count)
{
    if (!out || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    char dir_path[128];
    esp_err_t path_err = get_playset_dir(dir_path, sizeof(dir_path));
    if (path_err != ESP_OK) {
        return path_err;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return ESP_OK;
    }

    struct dirent *ent;
    size_t count = 0;
    const char *suffix = ".playset";
    size_t suffix_len = strlen(suffix);
    const char *prefix = "ps_";
    size_t prefix_len = 3;

    while ((ent = readdir(dir)) != NULL && count < max) {
        size_t fname_len = strlen(ent->d_name);
        if (fname_len <= suffix_len + prefix_len) continue;
        if (strncmp(ent->d_name, prefix, prefix_len) != 0) continue;
        if (strcmp(ent->d_name + fname_len - suffix_len, suffix) != 0) continue;

        // Skip .tmp files
        if (fname_len > 4 && strcmp(ent->d_name + fname_len - 4, ".tmp") == 0) continue;

        // Read header directly to get the playset name
        char fpath[256];
        snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, ent->d_name);

        FILE *f = fopen(fpath, "rb");
        if (!f) continue;

        playset_header_t header;
        if (fread(&header, sizeof(header), 1, f) != 1) {
            fclose(f);
            continue;
        }
        fclose(f);

        if (header.magic != PLAYSET_MAGIC || header.version != PLAYSET_VERSION) {
            // Old format or corrupt -- delete silently
            unlink(fpath);
            continue;
        }

        header.name[sizeof(header.name) - 1] = '\0';
        if (header.name[0] == '\0') continue;
        if (header.channel_count == 0 || header.channel_count > PS_MAX_CHANNELS) continue;

        playset_list_entry_t *entry = &out[count];
        strlcpy(entry->name, header.name, sizeof(entry->name));
        entry->channel_count = header.channel_count;
        count++;
    }

    closedir(dir);
    *out_count = count;
    return ESP_OK;
}
