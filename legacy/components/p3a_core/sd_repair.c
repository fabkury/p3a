// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_repair.c
 * @brief Self-healing for card-inflicted FAT32 damage. See sd_repair.h.
 */

#include "sd_repair.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "sd_repair";

#define SD_MOUNT_PREFIX "/sdcard"
#define LOST_DIR_NAME   "lost"
// The probe name is long on purpose: it needs 6 contiguous directory
// entries (5 LFN + 1 SFN), like the artwork names p3a writes. A short name
// fits in a single stray free slot ahead of the junk and would report a
// phantom directory as healthy (seen on vault/5/51).
#define PROBE_NAME      ".sdrp-probe-do-not-keep-0123456789abcdef0123456789.tmp"
// A readdir() scan that reaches this many entries, or meets a name with
// bytes outside printable ASCII, is looking at foreign data. p3a directories
// hold a few hundred entries at most and only ASCII names.
#define SCAN_MAX_ENTRIES 2048
#define NVS_NAMESPACE   "sd_repair"
#define NVS_KEY_TOTAL   "total"
#define REPAIR_PATH_MAX 520   // matches FS_ATOMIC_PATH_MAX (longest SD path seen + suffixes)

// FAT mirror check: 64 KB per copy per step (128 sectors), 128 B aligned in
// PSRAM (SDMMC DMA on the P4 wants cache-line alignment, jitter fix 1).
#define FAT_STEP_SECTORS 128
#define FAT_BUF_ALIGN    128

static sd_repair_fat_stats_t s_fat_stats;
static uint32_t s_boot_repairs = 0;
static uint32_t s_total_repairs = 0;
static bool s_total_loaded = false;
static bool s_overlay_armed = false;     // latch: the notice was armed this boot (a format clears it)
static bool s_overlay_pending = false;   // armed and not yet taken by the render loop
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// ----------------------------------------------------------------------------
// Counters (NVS-persisted total)
// ----------------------------------------------------------------------------

static void load_total(void)
{
    if (s_total_loaded) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        if (nvs_get_u32(h, NVS_KEY_TOTAL, &v) == ESP_OK) {
            s_total_repairs = v;
        }
        nvs_close(h);
    }
    s_total_loaded = true;
}

static void store_total(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u32(h, NVS_KEY_TOTAL, v) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// Caller holds s_lock. Only the first repair of the boot arms the on-screen
// notice; later ones move the counters, not the screen (a faulty card can
// need several repairs in the minutes after boot).
static void arm_overlay_once_locked(void)
{
    if (!s_overlay_armed) {
        s_overlay_armed = true;
        s_overlay_pending = true;
    }
}

static void count_repair(void)
{
    load_total();
    portENTER_CRITICAL(&s_lock);
    s_boot_repairs++;
    uint32_t total = ++s_total_repairs;
    arm_overlay_once_locked();
    portEXIT_CRITICAL(&s_lock);
    store_total(total);
}

bool sd_repair_take_pending_overlay(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_lock);
    if (s_overlay_pending) {
        s_overlay_pending = false;
        pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

uint32_t sd_repair_count_boot(void)
{
    return s_boot_repairs;
}

uint32_t sd_repair_count_total(void)
{
    load_total();
    return s_total_repairs;
}

void sd_repair_reset_total(void)
{
    portENTER_CRITICAL(&s_lock);
    s_total_repairs = 0;
    s_total_loaded = true;
    s_overlay_armed = false;    // fresh filesystem: a new repair is news again
    portEXIT_CRITICAL(&s_lock);
    store_total(0);
}

// ----------------------------------------------------------------------------
// Directory probe / quarantine
// ----------------------------------------------------------------------------

static bool is_sd_path(const char *path)
{
    return path && strncmp(path, SD_MOUNT_PREFIX, strlen(SD_MOUNT_PREFIX)) == 0;
}

static bool dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * Fallback when the create probe cannot run (FatFS refused the create):
 * enumerate the directory. Junk shows up as non-ASCII names, or as a table
 * that never ends (a chain running through FAT junk gives FatFS a 65536-entry
 * directory and creates fail with FR_DENIED / EACCES).
 */
static bool dir_scan_looks_damaged(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d) {
        return false;
    }
    bool damaged = false;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (++n >= SCAN_MAX_ENTRIES) {
            ESP_LOGW(TAG, "directory table does not end (%d+ entries): %s", n, dir_path);
            damaged = true;
            break;
        }
        for (const unsigned char *c = (const unsigned char *)de->d_name; *c; c++) {
            if (*c < 0x20 || *c > 0x7E) {
                ESP_LOGW(TAG, "directory holds foreign entries (non-ASCII name at #%d): %s", n, dir_path);
                damaged = true;
                break;
            }
        }
        if (damaged) {
            break;
        }
    }
    closedir(d);
    return damaged;
}

bool sd_repair_dir_is_damaged(const char *dir_path)
{
    if (!is_sd_path(dir_path)) {
        return false;
    }
    // Path buffers live on the heap: this runs on whichever task hit the
    // failure (download workers included), and their stacks are small.
    char *probe = malloc(REPAIR_PATH_MAX);
    if (!probe) {
        return false;
    }
    bool damaged = false;
    int n = snprintf(probe, REPAIR_PATH_MAX, "%s/" PROBE_NAME, dir_path);
    if (n < 0 || n >= REPAIR_PATH_MAX) {
        goto out;
    }
    unlink(probe);  // stale marker from an interrupted probe (a no-op on a phantom)

    FILE *f = fopen(probe, "wb");
    if (!f) {
        // FatFS refused the create. ENOENT/EACCES here can still be junk (a
        // table that never ends); let the enumeration decide. Anything else
        // (ENOSPC, EIO) is not our business.
        int e = errno;
        if (e == ENOENT || e == EACCES) {
            damaged = dir_scan_looks_damaged(dir_path);
        }
        goto out;
    }
    fclose(f);

    struct stat st;
    if (stat(probe, &st) == 0) {
        unlink(probe);
        goto out;       // healthy: what we created is visible
    }
    int e = errno;
    ESP_LOGW(TAG, "directory hides its own new entries (phantom zone): %s (stat errno=%d)", dir_path, e);
    damaged = (e == ENOENT);
out:
    free(probe);
    return damaged;
}

esp_err_t sd_repair_quarantine_dir(const char *dir_path)
{
    if (!is_sd_path(dir_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *root = sd_path_get_root();
    size_t root_len = strlen(root);
    // Never quarantine the SD root or anything above it: lost/ lives inside it.
    if (strncmp(dir_path, root, root_len) != 0 || dir_path[root_len] != '/' || dir_path[root_len + 1] == '\0') {
        ESP_LOGE(TAG, "refusing to quarantine %s (not below SD root %s)", dir_path, root);
        return ESP_ERR_INVALID_ARG;
    }

    // Two heap path buffers: lost/ directory, then the destination name.
    char *lost_dir = malloc(REPAIR_PATH_MAX);
    char *dest = malloc(REPAIR_PATH_MAX);
    esp_err_t err = ESP_FAIL;
    if (!lost_dir || !dest) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    snprintf(lost_dir, REPAIR_PATH_MAX, "%s/" LOST_DIR_NAME, root);
    if (!dir_exists(lost_dir) && mkdir(lost_dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "cannot create %s (errno=%d)", lost_dir, errno);
        goto out;
    }

    // Name the quarantined copy after its path below the root, slashes
    // replaced by underscores (vault/22/55 -> vault_22_55-<n>), so a person
    // looking at lost/ can tell where each folder came from.
    char base[96];
    strlcpy(base, dir_path + root_len + 1, sizeof(base));
    for (char *c = base; *c; c++) {
        if (*c == '/') {
            *c = '_';
        }
    }
    load_total();
    for (uint32_t seq = s_total_repairs + 1;; seq++) {
        int n = snprintf(dest, REPAIR_PATH_MAX, "%s/%s-%lu", lost_dir, base, (unsigned long)seq);
        if (n < 0 || n >= REPAIR_PATH_MAX) {
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
        struct stat st;
        if (stat(dest, &st) != 0) {
            break;
        }
        if (seq > s_total_repairs + 1000) {
            goto out;
        }
    }

    if (rename(dir_path, dest) != 0) {
        ESP_LOGE(TAG, "quarantine rename failed: %s -> %s (errno=%d)", dir_path, dest, errno);
        goto out;
    }
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "recreate after quarantine failed: %s (errno=%d)", dir_path, errno);
        goto out;
    }
    count_repair();
    ESP_LOGE(TAG, "REPAIRED damaged directory: %s moved to %s and recreated empty "
                  "(repair %lu this boot, %lu total). The card is corrupting data.",
             dir_path, dest, (unsigned long)s_boot_repairs, (unsigned long)s_total_repairs);
    err = ESP_OK;
out:
    free(lost_dir);
    free(dest);
    return err;
}

esp_err_t sd_repair_heal_for_write(const char *file_path)
{
    if (!is_sd_path(file_path)) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *root = sd_path_get_root();
    size_t root_len = strlen(root);
    if (strncmp(file_path, root, root_len) != 0 || file_path[root_len] != '/') {
        return ESP_ERR_NOT_FOUND;  // outside the configured root: not ours to heal
    }

    // Three heap path buffers: the target directory, the prefix being
    // walked, and that prefix's parent.
    char *dir = malloc(REPAIR_PATH_MAX);
    char *probe = malloc(REPAIR_PATH_MAX);
    char *parent = malloc(REPAIR_PATH_MAX);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    bool repaired = false;
    if (!dir || !probe || !parent) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    strlcpy(dir, file_path, REPAIR_PATH_MAX);
    char *last = strrchr(dir, '/');
    if (!last || last == dir) {
        goto out;
    }
    *last = '\0';
    if (strlen(dir) <= root_len) {
        // File directly under the SD root: the root itself cannot be
        // quarantined; let the caller fail normally.
        goto out;
    }

    // Walk from the root down. At the first component that does not exist,
    // decide whether its parent is hiding it (damaged) or it simply never
    // existed (create it). The deepest existing directory is probed last.
    strlcpy(probe, dir, REPAIR_PATH_MAX);
    char *p = probe + root_len + 1;
    while (true) {
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
        }
        // probe now holds a prefix directory path
        if (!dir_exists(probe)) {
            strlcpy(parent, probe, REPAIR_PATH_MAX);
            char *ps = strrchr(parent, '/');
            if (ps) {
                *ps = '\0';
            }
            if (strlen(parent) > root_len && sd_repair_dir_is_damaged(parent)) {
                if (sd_repair_quarantine_dir(parent) != ESP_OK) {
                    err = ESP_FAIL;
                    goto out;
                }
                repaired = true;
            }
            if (mkdir(probe, 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir %s failed during heal (errno=%d)", probe, errno);
                goto out;
            }
        }
        if (!slash) {
            break;
        }
        *slash = '/';
        p = slash + 1;
    }

    // The target directory exists (or was just created): probe it.
    if (!repaired && sd_repair_dir_is_damaged(dir)) {
        if (sd_repair_quarantine_dir(dir) != ESP_OK) {
            err = ESP_FAIL;
            goto out;
        }
        repaired = true;
    }
out:
    free(dir);
    free(probe);
    free(parent);
    return repaired ? ESP_OK : err;
}

// ----------------------------------------------------------------------------
// FAT mirror check
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t part_start;    // LBA of the volume
    uint32_t bps;           // bytes per sector (must equal card sector size)
    uint32_t fat_start;     // LBA of FAT1
    uint32_t fat_size;      // sectors per FAT
    uint32_t n_fats;
    uint32_t n_clusters;    // data clusters
} fat_geom_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static esp_err_t read_geometry(sdmmc_card_t *card, uint8_t *sector_buf, fat_geom_t *g)
{
    const uint32_t ss = (uint32_t)card->csd.sector_size;
    esp_err_t err = sdmmc_read_sectors(card, sector_buf, 0, 1);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t part_start = 0;
    if (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA && sector_buf[0] != 0xEB && sector_buf[0] != 0xE9) {
        const uint8_t *pe = sector_buf + 0x1BE;   // first partition entry
        if (pe[4] != 0) {
            part_start = rd32(pe + 8);
        }
        err = sdmmc_read_sectors(card, sector_buf, part_start, 1);
        if (err != ESP_OK) {
            return err;
        }
    }
    const uint8_t *b = sector_buf;
    uint32_t bps = rd16(b + 11);
    uint32_t spc = b[13];
    uint32_t rsvd = rd16(b + 14);
    uint32_t nfats = b[16];
    uint32_t fatsz16 = rd16(b + 22);
    uint32_t tot16 = rd16(b + 19);
    uint32_t tot32 = rd32(b + 32);
    uint32_t fatsz32 = rd32(b + 36);
    if (bps != ss || spc == 0 || nfats != 2 || fatsz16 != 0 || fatsz32 == 0 || rsvd == 0) {
        ESP_LOGW(TAG, "FAT mirror check skipped: geometry unsupported (bps=%lu spc=%lu fats=%lu fatsz16=%lu fatsz32=%lu)",
                 (unsigned long)bps, (unsigned long)spc, (unsigned long)nfats, (unsigned long)fatsz16, (unsigned long)fatsz32);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint32_t total = tot16 ? tot16 : tot32;
    if (total <= rsvd + nfats * fatsz32) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    g->part_start = part_start;
    g->bps = bps;
    g->fat_start = part_start + rsvd;
    g->fat_size = fatsz32;
    g->n_fats = nfats;
    g->n_clusters = (total - rsvd - nfats * fatsz32) / spc;
    return ESP_OK;
}

/**
 * Classify one FAT32 sector.
 *  0 = plausible: every entry is free, EOC, bad, or a valid cluster number.
 *  1 = suspicious: plausible values, but the chain links point far away from
 *      the sector's own cluster range (a FAT fragment misplaced from elsewhere).
 *  2 = implausible: entries outside the volume (foreign data).
 */
static int classify_fat_sector(const uint8_t *sec, uint32_t bps, uint32_t first_cluster, uint32_t n_clusters)
{
    const uint32_t entries = bps / 4;
    uint32_t links = 0, near = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t v = rd32(sec + 4 * i) & 0x0FFFFFFF;
        if (v == 0 || v >= 0x0FFFFFF7) {
            continue;
        }
        if (v < 2 || v >= n_clusters + 2) {
            return 2;
        }
        links++;
        uint32_t idx = first_cluster + i;
        uint32_t d = v > idx ? v - idx : idx - v;
        if (d <= 4096) {
            near++;
        }
    }
    if (links >= 8 && near * 5 < links) {
        return 1;   // fewer than 20% of the links are local: a fragment from elsewhere
    }
    return 0;
}

esp_err_t sd_repair_fat_mirror(sdmmc_card_t *card, bool fix, sd_repair_fat_stats_t *out)
{
    memset(&s_fat_stats, 0, sizeof(s_fat_stats));
    if (!card) {
        if (out) *out = s_fat_stats;
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t t0 = esp_timer_get_time();
    const uint32_t ss = (uint32_t)card->csd.sector_size;
    const size_t buf_bytes = (size_t)FAT_STEP_SECTORS * ss;
    uint8_t *b1 = heap_caps_aligned_alloc(FAT_BUF_ALIGN, buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *b2 = heap_caps_aligned_alloc(FAT_BUF_ALIGN, buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_OK;
    if (!b1 || !b2) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    fat_geom_t g;
    err = read_geometry(card, b1, &g);
    if (err != ESP_OK) {
        goto done;
    }
    s_fat_stats.fat_sectors = g.fat_size;

    for (uint32_t sec = 0; sec < g.fat_size; sec += FAT_STEP_SECTORS) {
        uint32_t n = g.fat_size - sec;
        if (n > FAT_STEP_SECTORS) n = FAT_STEP_SECTORS;
        err = sdmmc_read_sectors(card, b1, g.fat_start + sec, n);
        if (err != ESP_OK) goto done;
        err = sdmmc_read_sectors(card, b2, g.fat_start + g.fat_size + sec, n);
        if (err != ESP_OK) goto done;
        if (memcmp(b1, b2, n * ss) == 0) {
            continue;
        }
        for (uint32_t k = 0; k < n; k++) {
            uint8_t *s1 = b1 + k * ss;
            uint8_t *s2 = b2 + k * ss;
            if (memcmp(s1, s2, ss) == 0) {
                continue;
            }
            s_fat_stats.mismatched++;
            uint32_t first_cluster = (sec + k) * (ss / 4);
            int c1 = classify_fat_sector(s1, ss, first_cluster, g.n_clusters);
            int c2 = classify_fat_sector(s2, ss, first_cluster, g.n_clusters);
            if (c1 == 0 && c2 == 0) {
                continue;   // both sane, FAT1 is FatFS's truth; it resyncs FAT2 on its next write
            }
            if (c1 == c2) {
                s_fat_stats.unrepairable++;
                ESP_LOGW(TAG, "FAT sector %lu: both copies damaged (class %d), left alone",
                         (unsigned long)(sec + k), c1);
                continue;
            }
            // Exactly one copy is worse: restore it from the other.
            bool fat1_bad = c1 > c2;
            uint32_t dst_lba = fat1_bad ? g.fat_start + sec + k : g.fat_start + g.fat_size + sec + k;
            const uint8_t *src = fat1_bad ? s2 : s1;
            ESP_LOGE(TAG, "FAT sector %lu: %s damaged (class %d vs %d)%s",
                     (unsigned long)(sec + k), fat1_bad ? "FAT1" : "FAT2",
                     fat1_bad ? c1 : c2, fat1_bad ? c2 : c1, fix ? ", restoring from the other copy" : "");
            if (fix) {
                // Copy into the damaged buffer slot (128 B aligned) and write it out.
                uint8_t *dst = fat1_bad ? s1 : s2;
                memcpy(dst, src, ss);
                err = sdmmc_write_sectors(card, dst, dst_lba, 1);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "FAT repair write failed at LBA %lu: %s", (unsigned long)dst_lba, esp_err_to_name(err));
                    goto done;
                }
            }
            if (fat1_bad) s_fat_stats.repaired_fat1++; else s_fat_stats.repaired_fat2++;
        }
    }
    s_fat_stats.ran = true;
    if (fix && (s_fat_stats.repaired_fat1 || s_fat_stats.repaired_fat2)) {
        portENTER_CRITICAL(&s_lock);
        arm_overlay_once_locked();  // a rewritten FAT is a repair the user should hear about
        portEXIT_CRITICAL(&s_lock);
    }

done:
    s_fat_stats.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (b1) heap_caps_free(b1);
    if (b2) heap_caps_free(b2);
    if (s_fat_stats.ran) {
        if (s_fat_stats.repaired_fat1 || s_fat_stats.repaired_fat2 || s_fat_stats.unrepairable) {
            ESP_LOGE(TAG, "FAT mirror check: %lu sectors, %lu mismatched, FAT1 restored %lu, FAT2 restored %lu, unrepairable %lu (%lu ms)%s",
                     (unsigned long)s_fat_stats.fat_sectors, (unsigned long)s_fat_stats.mismatched,
                     (unsigned long)s_fat_stats.repaired_fat1, (unsigned long)s_fat_stats.repaired_fat2,
                     (unsigned long)s_fat_stats.unrepairable, (unsigned long)s_fat_stats.elapsed_ms,
                     fix ? "" : " [check only]");
        } else {
            ESP_LOGI(TAG, "FAT mirror check OK: %lu sectors, %lu benign mismatches (%lu ms)",
                     (unsigned long)s_fat_stats.fat_sectors, (unsigned long)s_fat_stats.mismatched,
                     (unsigned long)s_fat_stats.elapsed_ms);
        }
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "FAT mirror check did not run: %s", esp_err_to_name(err));
    }
    if (out) *out = s_fat_stats;
    return err;
}

const sd_repair_fat_stats_t *sd_repair_fat_stats(void)
{
    return &s_fat_stats;
}
