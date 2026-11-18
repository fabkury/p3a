// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The pure half of sd_path.h: the root, name sanitizing, the shard layout.
// No IDF includes; host-tested in tests/host/storage/test_sd_path.c.

#include "sd_path.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MOUNT_POINT     "/sdcard"
#define MOUNT_POINT_LEN 7

_Static_assert(SD_SHARD_DEPTH >= 1 && SD_SHARD_DEPTH <= 8,
               "SD_SHARD_DEPTH must fit within the 8 bytes of the 64-bit shard hash");
_Static_assert(sizeof(SD_PATH_DEFAULT_ROOT) <= SD_PATH_ROOT_MAX_LEN, "the default root must fit");

// Fixed for the boot once sd_path_init() has run.
static char s_root[SD_PATH_ROOT_MAX_LEN] = SD_PATH_DEFAULT_ROOT;

static int has_mount_point(const char *root)
{
    return strncmp(root, MOUNT_POINT "/", MOUNT_POINT_LEN + 1) == 0;
}

esp_err_t sd_path_validate_root(const char *root)
{
    if (!root || root[0] != '/' || root[1] == '\0' || strstr(root, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // "/sdcard/" alone names no folder.
    if (has_mount_point(root) && root[MOUNT_POINT_LEN + 1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t resolved_len = strlen(root) + (has_mount_point(root) ? 0 : MOUNT_POINT_LEN);
    if (resolved_len >= SD_PATH_ROOT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t sd_path_init(const char *configured_root)
{
    if (!configured_root || configured_root[0] == '\0') {
        snprintf(s_root, sizeof(s_root), "%s", SD_PATH_DEFAULT_ROOT);
        return ESP_OK;
    }
    esp_err_t err = sd_path_validate_root(configured_root);
    if (err != ESP_OK) {
        snprintf(s_root, sizeof(s_root), "%s", SD_PATH_DEFAULT_ROOT);
        return err;
    }
    // Cannot truncate: the validator capped the resolved length.
    snprintf(s_root, sizeof(s_root), "%s%s", has_mount_point(configured_root) ? "" : MOUNT_POINT, configured_root);
    return ESP_OK;
}

const char *sd_path_get_root(void)
{
    return s_root;
}

esp_err_t sd_path_get_subdir(const char *subdir, char *out_path, size_t out_len)
{
    if (!subdir || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(out_path, out_len, "%s/%s", s_root, subdir);
    return (written < 0 || (size_t)written >= out_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t sd_path_get_animations(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("animations", out_path, out_len);
}

esp_err_t sd_path_get_temporary(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("temporary", out_path, out_len);
}

// One file-name character to its FAT-safe form. The only place this mapping
// lives: sd_path_sanitize_filename() and the shard hash must never disagree,
// because the shard folders are derived from the hash of the sanitized name.
static inline char sanitize_char(char c)
{
    switch (c) {
    case ':':
    case '/':
    case '\\':
    case '?':
    case '*':
    case '"':
    case '<':
    case '>':
    case '|':
        return '_';
    default:
        return c;
    }
}

// 64-bit FNV-1a over the SANITIZED leaf name, finished with fmix64
// (MurmurHash3's avalanche mix) so that every byte of the result is evenly
// spread even for short or similar names.
//
// ON-DISK FORMAT: these constants decide where every stored file lives.
static uint64_t shard_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)sanitize_char(*p);
        h *= 0x100000001b3ULL;  // FNV prime
    }
    h ^= h >> 33;  // fmix64
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

void sd_path_sanitize_filename(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < out_len; i++) {
        out[o++] = sanitize_char(in[i]);
    }
    out[o] = '\0';
}

esp_err_t sd_path_build_sharded(const char *base, const char *leaf_name, const char *ext, char *out_path,
                                size_t out_len)
{
    if (!base || !leaf_name || leaf_name[0] == '\0' || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ext) {
        ext = "";
    }
    const uint64_t hash = shard_hash(leaf_name);

    // "{base}", then one "/{dir}" per shard level, then the sanitized leaf and
    // the extension. The loop, not a literal "%u/%u", is what makes
    // SD_SHARD_DEPTH the single knob for the layout.
    int n = snprintf(out_path, out_len, "%s", base);
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t pos = (size_t)n;

    for (int i = 0; i < SD_SHARD_DEPTH; i++) {
        unsigned dir = (unsigned)((hash >> (8 * i)) & SD_SHARD_MASK);
        n = snprintf(out_path + pos, out_len - pos, "/%u", dir);
        if (n < 0 || (size_t)n >= out_len - pos) {
            return ESP_ERR_INVALID_SIZE;
        }
        pos += (size_t)n;
    }

    // The leaf, sanitized with the same mapping the hash consumed.
    if (pos + 1 >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    out_path[pos++] = '/';
    for (const char *p = leaf_name; *p; p++) {
        if (pos + 1 >= out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        out_path[pos++] = sanitize_char(*p);
    }

    n = snprintf(out_path + pos, out_len - pos, "%s", ext);
    if (n < 0 || (size_t)n >= out_len - pos) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
