// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file playset_store.c
 * @brief Playset storage implementation
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

// Base directory for playset files
#define PLAYSET_DIR "/sdcard/p3a/channel"

/**
 * @brief Build the full file path for a playset
 */
static void build_path(const char *name, char *out_path, size_t path_len)
{
    snprintf(out_path, path_len, PLAYSET_DIR "/%s.playset", name);
}

/**
 * @brief Build the temporary file path for atomic write
 */
static void build_tmp_path(const char *name, char *out_path, size_t path_len)
{
    snprintf(out_path, path_len, PLAYSET_DIR "/%s.playset.tmp", name);
}

/**
 * @brief Calculate CRC32 over header and entries
 *
 * Zeros the checksum field before calculating.
 */
static uint32_t calculate_checksum(const playset_header_t *header,
                                    const playset_channel_entry_t *entries,
                                    size_t entry_count)
{
    // Create a copy of header with checksum zeroed
    playset_header_t hdr_copy = *header;
    hdr_copy.checksum = 0;

    // Calculate CRC over header
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&hdr_copy, sizeof(hdr_copy));

    // Continue CRC over entries
    if (entries && entry_count > 0) {
        crc = esp_rom_crc32_le(crc, (const uint8_t *)entries,
                               entry_count * sizeof(playset_channel_entry_t));
    }

    return crc;
}

/**
 * @brief Ensure the playset directory exists
 */
static esp_err_t ensure_directory(void)
{
    struct stat st;
    if (stat(PLAYSET_DIR, &st) == 0) {
        return ESP_OK;
    }

    // Create parent directories if needed
    if (mkdir("/sdcard/p3a", 0755) != 0 && errno != EEXIST) {
        // Ignore error, parent may exist
    }

    if (mkdir(PLAYSET_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create directory %s: %s", PLAYSET_DIR, strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t playset_store_save(const char *name, const ps_scheduler_command_t *cmd)
{
    if (!name || !cmd || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cmd->channel_count == 0 || cmd->channel_count > PS_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel count: %zu", cmd->channel_count);
        return ESP_ERR_INVALID_ARG;
    }

    // Ensure directory exists
    esp_err_t err = ensure_directory();
    if (err != ESP_OK) {
        return err;
    }

    char path[128];
    char tmp_path[128];
    build_path(name, path, sizeof(path));
    build_tmp_path(name, tmp_path, sizeof(tmp_path));

    // Build header
    playset_header_t header = {0};
    header.magic = PLAYSET_MAGIC;
    header.version = PLAYSET_VERSION;
    header.flags = 0;
    header.exposure_mode = (uint8_t)cmd->exposure_mode;
    header.pick_mode = (uint8_t)cmd->pick_mode;
    header.channel_count = (uint16_t)cmd->channel_count;

    // Build entries
    playset_channel_entry_t *entries = calloc(cmd->channel_count, sizeof(playset_channel_entry_t));
    if (!entries) {
        ESP_LOGE(TAG, "Failed to allocate entries");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < cmd->channel_count; i++) {
        const ps_channel_spec_t *src = &cmd->channels[i];
        playset_channel_entry_t *dst = &entries[i];

        dst->type = (uint8_t)src->type;
        strncpy(dst->name, src->name, sizeof(dst->name) - 1);
        strncpy(dst->identifier, src->identifier, sizeof(dst->identifier) - 1);
        strncpy(dst->display_name, src->display_name, sizeof(dst->display_name) - 1);
        dst->weight = src->weight;
    }

    // Calculate checksum
    header.checksum = calculate_checksum(&header, entries, cmd->channel_count);

    // Write to temporary file
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s", tmp_path, strerror(errno));
        free(entries);
        return ESP_FAIL;
    }

    bool write_ok = true;

    // Write header
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to write header");
        write_ok = false;
    }

    // Write entries
    if (write_ok && fwrite(entries, sizeof(playset_channel_entry_t), cmd->channel_count, f) != cmd->channel_count) {
        ESP_LOGE(TAG, "Failed to write entries");
        write_ok = false;
    }

    // Flush and sync
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

    // Atomic rename: delete old file, rename tmp to final
    unlink(path);  // Ignore error if doesn't exist
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved playset '%s' with %zu channels", name, cmd->channel_count);
    return ESP_OK;
}

esp_err_t playset_store_load(const char *name, ps_scheduler_command_t *out_cmd)
{
    if (!name || !out_cmd || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    build_path(name, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    // Read header
    playset_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to read header");
        fclose(f);
        return ESP_FAIL;
    }

    // Validate magic
    if (header.magic != PLAYSET_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08lX (expected 0x%08X)",
                 (unsigned long)header.magic, PLAYSET_MAGIC);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    // Validate version
    if (header.version != PLAYSET_VERSION) {
        ESP_LOGW(TAG, "Version mismatch: %u (expected %u), deleting file",
                 header.version, PLAYSET_VERSION);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_VERSION;
    }

    // Validate channel count
    if (header.channel_count == 0 || header.channel_count > PS_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel count: %u", header.channel_count);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    // Read entries
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

    // Validate checksum
    uint32_t expected_crc = header.checksum;
    uint32_t actual_crc = calculate_checksum(&header, entries, header.channel_count);
    if (actual_crc != expected_crc) {
        ESP_LOGE(TAG, "CRC mismatch: 0x%08lX (expected 0x%08lX)",
                 (unsigned long)actual_crc, (unsigned long)expected_crc);
        free(entries);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    // Populate output command
    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->exposure_mode = (ps_exposure_mode_t)header.exposure_mode;
    out_cmd->pick_mode = (ps_pick_mode_t)header.pick_mode;
    out_cmd->channel_count = header.channel_count;

    for (size_t i = 0; i < header.channel_count; i++) {
        const playset_channel_entry_t *src = &entries[i];
        ps_channel_spec_t *dst = &out_cmd->channels[i];

        dst->type = (ps_channel_type_t)src->type;
        strncpy(dst->name, src->name, sizeof(dst->name) - 1);
        strncpy(dst->identifier, src->identifier, sizeof(dst->identifier) - 1);
        strncpy(dst->display_name, src->display_name, sizeof(dst->display_name) - 1);
        dst->weight = src->weight;
    }

    free(entries);

    ESP_LOGI(TAG, "Loaded playset '%s' with %zu channels", name, out_cmd->channel_count);
    return ESP_OK;
}

bool playset_store_exists(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return false;
    }

    char path[128];
    build_path(name, path, sizeof(path));

    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t playset_store_delete(const char *name)
{
    if (!name || strlen(name) == 0 || strlen(name) > PLAYSET_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    build_path(name, path, sizeof(path));

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

    DIR *dir = opendir(PLAYSET_DIR);
    if (!dir) {
        // Directory doesn't exist — not an error, just no playsets
        return ESP_OK;
    }

    struct dirent *ent;
    size_t count = 0;
    const char *suffix = ".playset";
    size_t suffix_len = strlen(suffix);

    while ((ent = readdir(dir)) != NULL && count < max) {
        // Match *.playset files
        size_t name_len = strlen(ent->d_name);
        if (name_len <= suffix_len) continue;
        if (strcmp(ent->d_name + name_len - suffix_len, suffix) != 0) continue;

        // Extract playset name (strip .playset suffix)
        size_t base_len = name_len - suffix_len;
        if (base_len > PLAYSET_MAX_NAME_LEN) continue;

        char name[PLAYSET_MAX_NAME_LEN + 1];
        memcpy(name, ent->d_name, base_len);
        name[base_len] = '\0';

        // Load to get metadata (heap-allocate: ps_scheduler_command_t is ~46KB)
        ps_scheduler_command_t *cmd = calloc(1, sizeof(ps_scheduler_command_t));
        if (!cmd) break;  // OOM - stop listing
        esp_err_t err = playset_store_load(name, cmd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Skipping '%s': load failed (%s)", name, esp_err_to_name(err));
            free(cmd);
            continue;
        }

        playset_list_entry_t *entry = &out[count];
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        entry->channel_count = cmd->channel_count;
        entry->exposure_mode = cmd->exposure_mode;
        entry->pick_mode = cmd->pick_mode;
        free(cmd);
        count++;
    }

    closedir(dir);
    *out_count = count;
    return ESP_OK;
}
