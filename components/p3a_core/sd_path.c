// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_path.c
 * @brief SD card path resolver: configurable root with subdirectory helpers
 */

#include "sd_path.h"
#include "config_store.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

_Static_assert(SD_SHARD_DEPTH >= 1 && SD_SHARD_DEPTH <= 8,
               "SD_SHARD_DEPTH must fit within the 8 bytes of the 64-bit shard hash");

static const char *TAG = "sd_path";

// Cached root path (loaded once at init, changes require reboot)
static char s_root_path[SD_PATH_ROOT_MAX_LEN] = SD_PATH_DEFAULT_ROOT;
static bool s_initialized = false;

esp_err_t sd_path_validate_root(const char *root_path)
{
    if (!root_path || root_path[0] == '\0') {
        ESP_LOGW(TAG, "Root path validation failed: empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (root_path[0] != '/') {
        ESP_LOGW(TAG, "Root path validation failed: must start with '/': %s", root_path);
        return ESP_ERR_INVALID_ARG;
    }
    if (root_path[1] == '\0') {
        ESP_LOGW(TAG, "Root path validation failed: cannot be just '/' - must specify at least one folder");
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(root_path, "..") != NULL) {
        ESP_LOGW(TAG, "Root path validation failed: must not contain '..': %s", root_path);
        return ESP_ERR_INVALID_ARG;
    }

    // Length check on the RESOLVED form: user-friendly paths get "/sdcard"
    // (7 chars) prepended at init, compatibility-form paths are used as-is.
    size_t resolved_len = strlen(root_path);
    if (strncmp(root_path, "/sdcard/", 8) != 0) {
        resolved_len += 7;
    }
    if (resolved_len >= SD_PATH_ROOT_MAX_LEN) {
        ESP_LOGW(TAG, "Root path validation failed: too long (resolved %zu, max %d): %s",
                 resolved_len, SD_PATH_ROOT_MAX_LEN - 1, root_path);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t sd_path_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Try to load from config store
    char *stored_path = NULL;
    esp_err_t err = config_store_get_sdcard_root(&stored_path);

    if (err == ESP_OK && stored_path != NULL && stored_path[0] != '\0') {
        if (sd_path_validate_root(stored_path) != ESP_OK) {
            // Reject-at-write lives in the /config handler; this is the boot
            // safety net for values written by older firmware or raw NVS edits.
            ESP_LOGW(TAG, "Invalid root path in config: %s - using default %s",
                     stored_path, SD_PATH_DEFAULT_ROOT);
            strlcpy(s_root_path, SD_PATH_DEFAULT_ROOT, sizeof(s_root_path));
        } else if (strncmp(stored_path, "/sdcard/", 8) == 0) {
            // Already has /sdcard prefix (compatibility form), use as-is
            strlcpy(s_root_path, stored_path, sizeof(s_root_path));
            ESP_LOGI(TAG, "Using configured root: %s", s_root_path);
        } else {
            // User-friendly path (e.g., /p3a), prepend /sdcard.
            // Cannot truncate: the validator capped the resolved length.
            snprintf(s_root_path, sizeof(s_root_path), "/sdcard%s", stored_path);
            ESP_LOGI(TAG, "Using configured root: %s (from user path: %s)", s_root_path, stored_path);
        }
        free(stored_path);
    } else {
        strlcpy(s_root_path, SD_PATH_DEFAULT_ROOT, sizeof(s_root_path));
        ESP_LOGI(TAG, "Using default root: %s", s_root_path);
    }

    s_initialized = true;
    return ESP_OK;
}

const char *sd_path_get_root(void)
{
    if (!s_initialized) {
        sd_path_init();
    }
    return s_root_path;
}

esp_err_t sd_path_get_subdir(const char *subdir, char *out_path, size_t out_len)
{
    if (!subdir || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *root = sd_path_get_root();
    int written = snprintf(out_path, out_len, "%s/%s", root, subdir);
    
    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t sd_path_get_animations(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("animations", out_path, out_len);
}

esp_err_t sd_path_get_vault(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("vault", out_path, out_len);
}

esp_err_t sd_path_get_channel(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("channel", out_path, out_len);
}

esp_err_t sd_path_get_playlists(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("playlists", out_path, out_len);
}

esp_err_t sd_path_get_temporary(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("temporary", out_path, out_len);
}

esp_err_t sd_path_get_giphy(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("giphy", out_path, out_len);
}

esp_err_t sd_path_get_museum(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("museum", out_path, out_len);
}

esp_err_t sd_path_get_pinned(char *out_path, size_t out_len)
{
    return sd_path_get_subdir("pinned", out_path, out_len);
}

static esp_err_t ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Path exists but is not a directory: %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Creating directory: %s", path);
    if (mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "Failed to create directory %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sd_path_ensure_directories(void)
{
    const char *root = sd_path_get_root();
    char path[128];
    esp_err_t err;

    // Create root directory first
    err = ensure_directory(root);
    if (err != ESP_OK) {
        return err;
    }

    // Create subdirectories
    const char *subdirs[] = {"animations", "vault", "channel", "playlists", "temporary", "giphy", "museum", "pinned"};
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, subdirs[i]);
        err = ensure_directory(path);
        if (err != ESP_OK) {
            // Log but continue - some directories might already exist
            ESP_LOGW(TAG, "Could not create %s", path);
        }
    }

    ESP_LOGI(TAG, "SD directories ensured under %s", root);
    return ESP_OK;
}

esp_err_t sd_path_ensure_parent_dirs(const char *filepath)
{
    if (!filepath || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char tmp[256];
    strlcpy(tmp, filepath, sizeof(tmp));
    char *slash = tmp + 1;  // skip leading "/"
    while ((slash = strchr(slash, '/')) != NULL) {
        *slash = '\0';
        struct stat st;
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir failed: %s (%s)", tmp, strerror(errno));
                return ESP_FAIL;
            }
        }
        *slash = '/';
        slash++;
    }
    return ESP_OK;
}

/**
 * Map one filename character to its FAT-safe form. Single source of truth
 * for both sd_path_sanitize_filename() and the shard hash / leaf emission
 * in sd_path_build_sharded() — the two must never disagree, because the
 * shard directories are derived from the hash of the sanitized leaf name.
 */
static inline char sanitize_char(char c)
{
    switch (c) {
        case ':': case '/': case '\\': case '?': case '*':
        case '"': case '<': case '>': case '|':
            return '_';
        default:
            return c;
    }
}

/**
 * 64-bit FNV-1a over the SANITIZED leaf name, finished with fmix64
 * (MurmurHash3's avalanche mix) so every byte of the result is uniformly
 * distributed even for short or similar keys.
 *
 * v1.0 ON-DISK FORMAT: these constants define where every cached file
 * lives. Never change them — see the SD_SHARD_DEPTH contract in sd_path.h.
 */
static uint64_t shard_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;             /* FNV offset basis */
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)sanitize_char(*p);
        h *= 0x100000001b3ULL;                      /* FNV prime */
    }
    h ^= h >> 33;                                   /* fmix64 */
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

void sd_path_sanitize_filename(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!in) { out[0] = '\0'; return; }
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_len; i++) {
        out[o++] = sanitize_char(in[i]);
    }
    out[o] = '\0';
}

esp_err_t sd_path_build_sharded(const char *base, const char *leaf_name,
                                const char *ext,
                                char *out_path, size_t out_len)
{
    if (!base || !leaf_name || leaf_name[0] == '\0' || !out_path || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ext) ext = "";

    uint64_t hash = shard_hash(leaf_name);

    // Emit "{base}", then one "/{dir}" per shard level (decimal 0..63 from
    // the low 6 bits of each successive hash byte), then the sanitized
    // leaf and the extension. The loop (not a literal "%u/%u") is what
    // makes SD_SHARD_DEPTH the single knob that drives the layout.
    size_t pos = 0;
    int n = snprintf(out_path, out_len, "%s", base);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    pos = (size_t)n;

    for (int i = 0; i < SD_SHARD_DEPTH; i++) {
        unsigned int dir = (unsigned int)((hash >> (8 * i)) & SD_SHARD_MASK);
        n = snprintf(out_path + pos, out_len - pos, "/%u", dir);
        if (n < 0 || (size_t)n >= out_len - pos) return ESP_ERR_INVALID_SIZE;
        pos += (size_t)n;
    }

    // Leaf: sanitized char-by-char with the same mapping the hash consumed,
    // so the shard location stays re-derivable from the on-disk name.
    if (pos + 1 >= out_len) return ESP_ERR_INVALID_SIZE;
    out_path[pos++] = '/';
    for (const char *p = leaf_name; *p; p++) {
        if (pos + 1 >= out_len) return ESP_ERR_INVALID_SIZE;
        out_path[pos++] = sanitize_char(*p);
    }

    n = snprintf(out_path + pos, out_len - pos, "%s", ext);
    if (n < 0 || (size_t)n >= out_len - pos) return ESP_ERR_INVALID_SIZE;

    return ESP_OK;
}

