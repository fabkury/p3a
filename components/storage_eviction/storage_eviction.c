// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file storage_eviction.c
 * @brief Age-based eviction of cached artwork and channel files from SD card
 *
 * When the SD card runs low on space, this module scans the vault and
 * giphy cache directories and deletes files whose mtime is older than
 * a progressively-shrinking threshold.  The threshold starts at
 * CONFIG_STORAGE_EVICTION_INITIAL_AGE_DAYS and halves on each pass
 * until the free-space target is met or the floor
 * (CONFIG_STORAGE_EVICTION_MIN_AGE_HOURS) is reached.
 *
 * Channel eviction deletes stale channel metadata files (.cache, .json,
 * .settings.json, .bin) from the channel directory when they haven't
 * been loaded for playback within CONFIG_CHANNEL_EVICTION_AGE_DAYS.
 * Channels in the active playset are always protected.
 */

#include "storage_eviction.h"
#include "play_scheduler.h"
#include "sd_path.h"
#include "sntp_sync.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "storage_evict";

#define TARGET_BYTES        ((uint64_t)CONFIG_STORAGE_EVICTION_TARGET_MIB * 1024ULL * 1024ULL)
#define HEADROOM_BYTES      ((uint64_t)CONFIG_STORAGE_EVICTION_HEADROOM_MIB * 1024ULL * 1024ULL)
#define STOP_BYTES          (TARGET_BYTES + HEADROOM_BYTES)
#define MIN_CARD_SIZE_BYTES ((uint64_t)CONFIG_STORAGE_EVICTION_MIN_CARD_SIZE_MIB * 1024ULL * 1024ULL)
#define INITIAL_AGE_S       ((time_t)CONFIG_STORAGE_EVICTION_INITIAL_AGE_DAYS * 86400)
#define MIN_AGE_S           ((time_t)CONFIG_STORAGE_EVICTION_MIN_AGE_HOURS * 3600)
#define CHANNEL_AGE_S       ((time_t)CONFIG_CHANNEL_EVICTION_AGE_DAYS * 86400)

/** Counters passed through the eviction scan */
typedef struct {
    uint32_t files_deleted;
    uint64_t bytes_freed;
} evict_stats_t;

/* --------------------------------------------------------------------- */
/*  Helpers                                                               */
/* --------------------------------------------------------------------- */

static bool is_artwork_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".webp") == 0 ||
            strcasecmp(dot, ".gif")  == 0 ||
            strcasecmp(dot, ".png")  == 0 ||
            strcasecmp(dot, ".jpg")  == 0);
}

/**
 * @brief Try to unlink a sidecar file (.404) next to an artwork
 */
static void remove_sidecar(const char *artwork_path, const char *suffix)
{
    char sidecar[280];
    int ret = snprintf(sidecar, sizeof(sidecar), "%s%s", artwork_path, suffix);
    if (ret > 0 && ret < (int)sizeof(sidecar)) {
        unlink(sidecar);  /* ignore errors — file may not exist */
    }
}

/* --------------------------------------------------------------------- */
/*  Free-space query                                                      */
/* --------------------------------------------------------------------- */

esp_err_t storage_eviction_get_free_space(uint64_t *out_free_bytes)
{
    return storage_eviction_get_storage_info(NULL, out_free_bytes);
}

esp_err_t storage_eviction_get_storage_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!out_total_bytes && !out_free_bytes) return ESP_ERR_INVALID_ARG;

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_info(\"/sdcard\") failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    if (out_total_bytes) *out_total_bytes = total_bytes;
    if (out_free_bytes)  *out_free_bytes  = free_bytes;
    return ESP_OK;
}

/* --------------------------------------------------------------------- */
/*  Leaf-directory scanner (3-level SHA256 sharding)                      */
/* --------------------------------------------------------------------- */

/**
 * @brief Scan a single leaf directory and delete old artwork files
 *
 * @param leaf_path  Full path to the leaf directory (e.g. vault/aa/bb/cc)
 * @param cutoff     Files with mtime < cutoff are eligible for deletion
 * @param stats      Running totals (updated in place)
 */
static void evict_leaf(const char *leaf_path, time_t cutoff, evict_stats_t *stats)
{
    DIR *d = opendir(leaf_path);
    if (!d) return;

    struct dirent *de;
    char filepath[280];

    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_artwork_ext(de->d_name)) continue;

        int ret = snprintf(filepath, sizeof(filepath), "%s/%s", leaf_path, de->d_name);
        if (ret < 0 || ret >= (int)sizeof(filepath)) continue;

        struct stat st;
        if (stat(filepath, &st) != 0) continue;

        if (st.st_mtime < cutoff) {
            uint64_t size = (uint64_t)st.st_size;
            if (unlink(filepath) == 0) {
                stats->files_deleted++;
                stats->bytes_freed += size;
                remove_sidecar(filepath, ".404");
            }
        }
    }

    closedir(d);
}

/**
 * @brief Recursively descend `levels` shard directories, then evict the leaf
 *
 * At levels == 0 we are at a leaf shard directory and scan it for old files;
 * otherwise we open `dir` and recurse into each child with one fewer level.
 * Driven by SD_SHARD_DEPTH so the walk depth always matches the layout the
 * sd_path_build_sharded() builders produce — change the depth in one place and
 * both the writers and this walker follow.
 */
static void evict_shard_tree(const char *dir, int levels, time_t cutoff, evict_stats_t *stats)
{
    if (levels <= 0) {
        evict_leaf(dir, cutoff, stats);
        /* Yield between leaf directories to avoid starving other tasks */
        vTaskDelay(1);
        return;
    }

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    char child[256];

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        int n = snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
        if (n < 0 || n >= (int)sizeof(child)) continue;

        evict_shard_tree(child, levels - 1, cutoff, stats);
    }

    closedir(d);
}

/**
 * @brief Walk a SHA256-sharded base directory and evict old files
 *
 * Directory structure: {base}/{xx}/{yy}/{zz}/{file} for SD_SHARD_DEPTH == 3.
 */
static void evict_from_base_dir(const char *base_path, time_t cutoff, evict_stats_t *stats)
{
    evict_shard_tree(base_path, SD_SHARD_DEPTH, cutoff, stats);
}

/**
 * @brief Walk a museum-rooted base directory and evict old files
 *
 * Museum vault layout has an extra museum_id segment at the top:
 *   /sdcard/p3a/museum/{museum_id}/{xx}/{yy}/{zz}/{file}
 * The existing evict_from_base_dir() walks SD_SHARD_DEPTH levels of SHA shards;
 * we just delegate to it once per museum_id directory found here.
 */
static void evict_museum_root(const char *base_path, time_t cutoff, evict_stats_t *stats)
{
    DIR *d = opendir(base_path);
    if (!d) return;

    struct dirent *e;
    char sub[160];

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        int r = snprintf(sub, sizeof(sub), "%s/%s", base_path, e->d_name);
        if (r < 0 || r >= (int)sizeof(sub)) continue;
        evict_from_base_dir(sub, cutoff, stats);
    }

    closedir(d);
}

/* --------------------------------------------------------------------- */
/*  Top-level eviction over vault, giphy, and museum caches               */
/* --------------------------------------------------------------------- */

static void evict_old_files(time_t cutoff, evict_stats_t *stats)
{
    char path[128];

    if (sd_path_get_vault(path, sizeof(path)) == ESP_OK) {
        evict_from_base_dir(path, cutoff, stats);
    }

    if (sd_path_get_giphy(path, sizeof(path)) == ESP_OK) {
        evict_from_base_dir(path, cutoff, stats);
    }

    if (sd_path_get_museum(path, sizeof(path)) == ESP_OK) {
        evict_museum_root(path, cutoff, stats);
    }
}

/* --------------------------------------------------------------------- */
/*  Public API                                                            */
/* --------------------------------------------------------------------- */

esp_err_t storage_eviction_check_and_run(void)
{
    /* Guard: require SNTP synchronisation for reliable mtime comparisons */
    if (!sntp_sync_is_synchronized()) {
        ESP_LOGD(TAG, "SNTP not synced, skipping eviction");
        return ESP_ERR_INVALID_STATE;
    }

    /* Guard: skip on cards too small to ever satisfy the stop watermark.
       Running eviction on a card this small would just churn the FS; we'd
       rather fill the card and stop downloading than thrash. Warn once so
       the condition is visible in the logs without spamming on every
       failed-download retry. */
    if (MIN_CARD_SIZE_BYTES > 0) {
        uint64_t total_bytes = 0;
        if (storage_eviction_get_storage_info(&total_bytes, NULL) == ESP_OK &&
            total_bytes < MIN_CARD_SIZE_BYTES) {
            static bool warned = false;
            if (!warned) {
                ESP_LOGW(TAG, "SD card total size %llu MiB < minimum %d MiB; eviction disabled",
                         (unsigned long long)(total_bytes / (1024 * 1024)),
                         CONFIG_STORAGE_EVICTION_MIN_CARD_SIZE_MIB);
                warned = true;
            }
            return ESP_OK;
        }
    }

    uint64_t free_bytes = 0;
    esp_err_t err = storage_eviction_get_free_space(&free_bytes);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    if (free_bytes >= TARGET_BYTES) {
        return ESP_OK;  /* trigger watermark not crossed */
    }

    ESP_LOGW(TAG, "Low disk space: %llu MiB free (trigger %d MiB), evicting until >= %llu MiB free",
             (unsigned long long)(free_bytes / (1024 * 1024)),
             CONFIG_STORAGE_EVICTION_TARGET_MIB,
             (unsigned long long)(STOP_BYTES / (1024 * 1024)));

    evict_stats_t stats = { 0, 0 };
    time_t now = time(NULL);
    time_t age_threshold = INITIAL_AGE_S;

    while (age_threshold >= MIN_AGE_S) {
        time_t cutoff = now - age_threshold;

        ESP_LOGI(TAG, "Eviction pass: age threshold %ld s (%ld h)",
                 (long)age_threshold, (long)(age_threshold / 3600));

        evict_old_files(cutoff, &stats);

        /* Re-check free space */
        err = storage_eviction_get_free_space(&free_bytes);
        if (err != ESP_OK) break;

        if (free_bytes >= STOP_BYTES) {
            ESP_LOGI(TAG, "Free space stop target reached (%llu MiB)",
                     (unsigned long long)(free_bytes / (1024 * 1024)));
            break;
        }

        /* Halve the threshold for the next pass */
        age_threshold /= 2;
    }

    ESP_LOGI(TAG, "Eviction done: %lu files deleted, %llu MiB freed",
             (unsigned long)stats.files_deleted,
             (unsigned long long)(stats.bytes_freed / (1024 * 1024)));

    return ESP_OK;
}

/* --------------------------------------------------------------------- */
/*  Channel eviction                                                      */
/* --------------------------------------------------------------------- */

static bool str_ends_with(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (suf_len > str_len) return false;
    return strcmp(str + str_len - suf_len, suffix) == 0;
}

/**
 * @brief Try to delete a companion file for a channel stem
 *
 * @return true if the file existed and was deleted
 */
static bool unlink_companion(const char *dir, const char *stem, const char *ext)
{
    char path[256];
    int ret = snprintf(path, sizeof(path), "%s/%s%s", dir, stem, ext);
    if (ret < 0 || ret >= (int)sizeof(path)) return false;
    return (unlink(path) == 0);
}

/**
 * @brief Check if a channel stem matches any of the active channel IDs
 */
static bool is_active_channel(const char *stem,
                              const char **active_ids, size_t active_count)
{
    for (size_t i = 0; i < active_count; i++) {
        if (strcmp(stem, active_ids[i]) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t channel_eviction_check_and_run(void)
{
    if (!sntp_sync_is_synchronized()) {
        ESP_LOGW(TAG, "Channel eviction: SNTP not synced, skipping");
        return ESP_ERR_INVALID_STATE;
    }

    char channel_dir[128];
    esp_err_t err = sd_path_get_channel(channel_dir, sizeof(channel_dir));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Channel eviction: failed to get channel dir");
        return err;
    }

    const char *active_ids[PS_MAX_CHANNELS];
    size_t active_count = play_scheduler_get_active_channel_ids(active_ids, PS_MAX_CHANNELS);

    time_t now = time(NULL);
    time_t cutoff = now - CHANNEL_AGE_S;

    ESP_LOGI(TAG, "Channel eviction: scanning %s (threshold %ld days, %zu active channels protected)",
             channel_dir, (long)CONFIG_CHANNEL_EVICTION_AGE_DAYS, active_count);

    DIR *d = opendir(channel_dir);
    if (!d) {
        ESP_LOGW(TAG, "Channel eviction: cannot open %s", channel_dir);
        return ESP_OK;
    }

    struct dirent *de;
    uint32_t channels_scanned = 0;
    uint32_t channels_evicted = 0;
    uint32_t channels_protected = 0;
    uint32_t channels_fresh = 0;
    uint32_t files_deleted = 0;

    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!str_ends_with(de->d_name, ".cache")) continue;

        /* Extract the stem (channel_id) by stripping ".cache" */
        size_t name_len = strlen(de->d_name);
        size_t stem_len = name_len - 6;  /* strlen(".cache") == 6 */
        if (stem_len == 0 || stem_len >= 64) continue;

        char stem[64];
        memcpy(stem, de->d_name, stem_len);
        stem[stem_len] = '\0';

        channels_scanned++;

        /* Protect the sdcard channel — compute its hash-based channel_id */
        {
            char sdcard_id[17];
            // Protect the canonical (offset=0) SDCARD channel cache. Non-zero-offset
            // SDCARD caches are derivable from the same local source and may be evicted
            // normally if they age out (the active-channel check below still protects
            // any sdcard slice that's currently in the active playset).
            ps_compute_channel_id(PS_CHANNEL_TYPE_SDCARD, "sdcard", "", 0, sdcard_id, sizeof(sdcard_id));
            if (strcmp(stem, sdcard_id) == 0) {
                ESP_LOGD(TAG, "  '%s': protected (sdcard)", stem);
                channels_protected++;
                continue;
            }
        }
        if (is_active_channel(stem, active_ids, active_count)) {
            ESP_LOGD(TAG, "  '%s': protected (active)", stem);
            channels_protected++;
            continue;
        }

        /* Check mtime of the .cache file */
        char cache_path[256];
        int ret = snprintf(cache_path, sizeof(cache_path), "%s/%s", channel_dir, de->d_name);
        if (ret < 0 || ret >= (int)sizeof(cache_path)) continue;

        struct stat st;
        if (stat(cache_path, &st) != 0) continue;

        long age_days = (long)(now - st.st_mtime) / 86400;
        if (st.st_mtime >= cutoff) {
            ESP_LOGD(TAG, "  '%s': fresh (%ld days old)", stem, age_days);
            channels_fresh++;
            continue;
        }

        ESP_LOGI(TAG, "  '%s': evicting (%ld days old)", stem, age_days);

        /* Evict: delete the .cache file and all companions */
        if (unlink(cache_path) == 0) files_deleted++;
        if (unlink_companion(channel_dir, stem, ".json")) files_deleted++;
        if (unlink_companion(channel_dir, stem, ".settings.json")) files_deleted++;
        if (unlink_companion(channel_dir, stem, ".bin")) files_deleted++;

        channels_evicted++;
    }

    closedir(d);

    ESP_LOGI(TAG, "Channel eviction done: %lu scanned, %lu protected, %lu fresh, %lu evicted (%lu files deleted)",
             (unsigned long)channels_scanned, (unsigned long)channels_protected,
             (unsigned long)channels_fresh, (unsigned long)channels_evicted,
             (unsigned long)files_deleted);

    return ESP_OK;
}
