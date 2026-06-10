// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_channel_utils.c
 * @brief Makapix utility helpers: UUID parsing, SHA256 vault paths, cache validation
 */

#include "makapix_channel_internal.h"
#include "makapix_channel_utils.h"
#include "channel_cache.h"
#include "sd_path.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "makapix_channel_utils";

// Extension strings for building file paths
const char *s_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

/**
 * @brief Validate a cache file by checking its header magic
 *
 * @param path Path to the cache file
 * @return true if file has valid CHANNEL_CACHE_MAGIC header
 */
static bool makapix_cache_file_is_valid(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint32_t magic = 0;
    size_t read = fread(&magic, sizeof(magic), 1, f);
    fclose(f);

    return (read == 1 && magic == CHANNEL_CACHE_MAGIC);
}

void makapix_cache_recover_and_cleanup(const char *cache_path)
{
    if (!cache_path) return;

    char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cache_path);

    struct stat st_cache = {0};
    struct stat st_tmp = {0};
    const bool have_cache = (stat(cache_path, &st_cache) == 0);
    const bool have_tmp = (stat(tmp_path, &st_tmp) == 0);

    if (!have_tmp) return;

    const bool tmp_valid = makapix_cache_file_is_valid(tmp_path);
    const bool cache_valid = have_cache ? makapix_cache_file_is_valid(cache_path) : false;

    if (!tmp_valid) {
        ESP_LOGW(TAG, "Cache temp file invalid; removing: %s (size=%ld)", tmp_path, (long)st_tmp.st_size);
        unlink(tmp_path);
        return;
    }

    if (!have_cache) {
        // Cache missing but temp exists and looks valid -> promote temp.
        ESP_LOGW(TAG, "Recovering missing channel cache from temp: %s -> %s", tmp_path, cache_path);
        if (rename(tmp_path, cache_path) != 0) {
            ESP_LOGE(TAG, "Failed to recover cache from temp: errno=%d (%s)", errno, strerror(errno));
        }
        return;
    }

    // Both exist. Choose which to keep.
    if (!cache_valid || st_tmp.st_mtime >= st_cache.st_mtime) {
        ESP_LOGW(TAG, "Promoting cache temp over existing channel cache (cache_valid=%d): %s -> %s",
                 (int)cache_valid, tmp_path, cache_path);
        (void)unlink(cache_path); // required on FATFS: rename() won't overwrite
        if (rename(tmp_path, cache_path) != 0) {
            ESP_LOGE(TAG, "Failed to promote cache temp: errno=%d (%s)", errno, strerror(errno));
        }
        return;
    }

    // Existing cache appears valid and newer than temp -> discard temp.
    ESP_LOGW(TAG, "Discarding stale cache temp file: %s", tmp_path);
    unlink(tmp_path);
}

bool uuid_to_bytes(const char *uuid_str, uint8_t *out_bytes)
{
    if (!uuid_str || !out_bytes) return false;
    
    size_t len = strlen(uuid_str);
    int out_idx = 0;
    
    for (size_t i = 0; i < len && out_idx < 16; i++) {
        if (uuid_str[i] == '-') continue;  // Skip hyphens
        
        if (i + 1 >= len) return false;
        
        unsigned int byte;
        char hex[3] = { uuid_str[i], uuid_str[i+1], '\0' };
        if (sscanf(hex, "%02x", &byte) != 1) return false;
        
        out_bytes[out_idx++] = (uint8_t)byte;
        i++;  // Skip second hex char (loop will increment again)
    }
    
    return (out_idx == 16);
}

void bytes_to_uuid(const uint8_t *bytes, char *out, size_t out_len)
{
    if (!bytes || !out || out_len < 37) return;
    
    snprintf(out, out_len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

esp_err_t storage_key_sha256(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) return ESP_ERR_INVALID_ARG;
    int ret = mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), out_sha256, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 failed (ret=%d)", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t makapix_build_vault_path(const char *vault_base, const char *storage_key,
                                   uint8_t ext_index, char *out, size_t out_len)
{
    if (!vault_base || !storage_key) return ESP_ERR_INVALID_ARG;
    const char *ext = s_ext_strings[(ext_index <= EXT_JPEG) ? ext_index : EXT_WEBP];
    return sd_path_build_sharded(vault_base, storage_key, ext, out, out_len);
}

esp_err_t makapix_build_remote_shard(const char *storage_key, char *out, size_t out_len)
{
    if (!storage_key || !out || out_len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t sha[32];
    if (storage_key_sha256(storage_key, sha) != ESP_OK) return ESP_FAIL;

    // "aa/bb" — no leading/trailing slash; depth = MAKAPIX_REMOTE_SHARD_DEPTH,
    // each level the low 6 bits of one SHA256 byte rendered as two lowercase
    // hex digits ("00".."3f").
    size_t pos = 0;
    for (int i = 0; i < MAKAPIX_REMOTE_SHARD_DEPTH; i++) {
        int n = snprintf(out + pos, out_len - pos, (i == 0) ? "%02x" : "/%02x",
                         (unsigned int)(sha[i] & MAKAPIX_REMOTE_SHARD_MASK));
        if (n < 0 || (size_t)n >= out_len - pos) return ESP_ERR_INVALID_SIZE;
        pos += (size_t)n;
    }
    return ESP_OK;
}

time_t parse_iso8601_utc(const char *s)
{
    if (!s || !*s) return 0;
    int year, month, day, hour, min, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec) != 6) {
        return 0;
    }
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    return mktime(&tm);
}

void build_index_path(const makapix_channel_t *ch, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s.bin", ch->channels_path, ch->channel_id);
}

void build_vault_path(const makapix_channel_t *ch,
                      const makapix_channel_entry_t *entry,
                      char *out, size_t out_len)
{
    // Convert stored bytes back to UUID string
    char storage_key[40];
    bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));

    if (makapix_build_vault_path(ch->vault_path, storage_key, entry->extension,
                                 out, out_len) != ESP_OK && out && out_len > 0) {
        out[0] = '\0';
    }
}

void build_vault_path_from_storage_key(const makapix_channel_t *ch, const char *storage_key,
                                        file_extension_t ext, char *out, size_t out_len)
{
    if (makapix_build_vault_path(ch->vault_path, storage_key, (uint8_t)ext,
                                 out, out_len) != ESP_OK && out && out_len > 0) {
        out[0] = '\0';
    }
}

file_extension_t detect_file_type(const char *url)
{
    if (!url) return EXT_WEBP;
    size_t len = strlen(url);
    // Check longer extensions first (e.g., .jpeg before .jpg)
    if (len >= 5 && strcasecmp(url + len - 5, ".webp") == 0) return EXT_WEBP;
    if (len >= 5 && strcasecmp(url + len - 5, ".jpeg") == 0) return EXT_JPEG;
    if (len >= 4 && strcasecmp(url + len - 4, ".gif") == 0) return EXT_GIF;
    if (len >= 4 && strcasecmp(url + len - 4, ".png") == 0) return EXT_PNG;
    if (len >= 4 && strcasecmp(url + len - 4, ".jpg") == 0) return EXT_JPEG;
    return EXT_WEBP;
}

uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                    uint32_t channel_override_ms,
                                    uint32_t playlist_or_artwork_ms)
{
    uint32_t eff = playlist_or_artwork_ms;
    if (channel_override_ms != 0) eff = channel_override_ms;
    if (global_override_ms != 0) eff = global_override_ms;
    if (eff == 0) eff = 30000;
    return eff;
}

