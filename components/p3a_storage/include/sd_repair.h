// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_repair.h
 * @brief Self-healing for FAT32 damage inflicted by the SD card itself.
 *
 * Background (requests/01-sd-card-intermittent-failure): some microSD cards,
 * typically after a power cut, store 16 KB pages at the wrong logical
 * address. FatFS then sees foreign data inside a directory cluster or inside
 * the first FAT copy. Two symptoms follow:
 *
 *  - A directory whose leading entries were overwritten: FatFS's lookup
 *    (dir_find) stops at the first 0x00 entry it meets in the foreign data,
 *    while allocation (dir_alloc) skips past it, so every file created there
 *    is a phantom: fopen() succeeds, stat()/rename() of the same name fail
 *    with ENOENT, and the SD-failure latch trips after three of them. The
 *    files that used to be listed in that directory are unreachable.
 *  - A FAT1 sector overwritten with junk while FAT2 still holds the mirror.
 *    FatFS reads only FAT1, so chains through that sector read garbage and
 *    the clusters it covers are lost to allocation.
 *
 * This module repairs both without a PC and without formatting:
 *
 *  - sd_repair_heal_for_write(): probes the directory a write is failing in
 *    (create a marker, stat it). A directory that hides what was just created
 *    is quarantined (moved to {root}/lost/) and recreated empty. Caches
 *    (vault, giphy, klipy, museum, channel) refill themselves; user content
 *    that lived there was already unreachable and stays available in lost/
 *    for a PC to inspect. Called from the write choke points (fs_atomic,
 *    sd_path_ensure_parent_dirs) on ENOENT, never speculatively.
 *  - sd_repair_fat_mirror(): at boot, right after mount and before anything
 *    touches the FAT, compares FAT1 with FAT2 sector by sector and restores
 *    a sector whose copy is implausible (values outside the volume) or
 *    suspicious (a plausible-looking fragment that belongs elsewhere in the
 *    FAT) from the other copy. Sectors where both copies look sane but
 *    differ are left alone (FatFS resyncs them on its next write).
 *
 * Every repair is counted (per boot and persisted in NVS) and surfaced in
 * /status and the web UI: a card that needs repairing is a card that is
 * losing data, and the user should know.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sd_protocol_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ran;                   /**< false: no card, unsupported geometry, or read failure */
    uint32_t fat_sectors;       /**< sectors per FAT copy */
    uint32_t mismatched;        /**< sectors where FAT1 != FAT2 */
    uint32_t repaired_fat1;     /**< FAT1 sectors restored from FAT2 */
    uint32_t repaired_fat2;     /**< FAT2 sectors restored from FAT1 */
    uint32_t unrepairable;      /**< mismatched sectors with no trustworthy copy */
    uint32_t elapsed_ms;
} sd_repair_fat_stats_t;

/**
 * Compare FAT1 with FAT2 and (when fix is true) restore damaged sectors from
 * the trustworthy copy. Raw sector I/O on the card: call only right after
 * mount, before any FatFS operation, or with the volume unmounted. The
 * stats are kept and readable later via sd_repair_fat_stats().
 */
esp_err_t sd_repair_fat_mirror(sdmmc_card_t *card, bool fix, sd_repair_fat_stats_t *out);

/** Stats of the last sd_repair_fat_mirror() run (zeroed, ran=false before). */
const sd_repair_fat_stats_t *sd_repair_fat_stats(void);

/**
 * Probe a directory: create a long-named marker file in it and stat it back.
 * Returns true when the directory hides its own new entries (phantom zone),
 * or, when FatFS refuses the create with ENOENT/EACCES, when an enumeration
 * shows foreign entries (non-ASCII names) or a table that never ends (a
 * chain through FAT junk). The marker is removed on healthy directories.
 * Paths outside the SD mount are always healthy.
 */
bool sd_repair_dir_is_damaged(const char *dir_path);

/**
 * Move dir_path to {root}/lost/{name}-{n} and recreate it empty. Refuses the
 * SD root itself and anything outside the SD mount.
 */
esp_err_t sd_repair_quarantine_dir(const char *dir_path);

/**
 * A write to file_path failed with ENOENT or EACCES: find out whether its
 * directory (or an ancestor) is damaged and repair it. Returns ESP_OK when something
 * was repaired and the caller may retry the write, ESP_ERR_NOT_FOUND when
 * nothing was wrong with the directories (the ENOENT has another cause), or
 * an error when a repair was needed but failed.
 */
esp_err_t sd_repair_heal_for_write(const char *file_path);

/**
 * For the render loop only: returns true exactly once after the first repair
 * of the boot (directory quarantine or boot-time FAT restore). Later repairs
 * of the same boot do not re-arm it, so the on-screen notice shows once per
 * boot; only sd_repair_reset_total() (a format) lets it arm again.
 */
bool sd_repair_take_pending_overlay(void);

/** Directory repairs since boot. */
uint32_t sd_repair_count_boot(void);

/** Directory repairs persisted across boots (NVS); reset by a format. */
uint32_t sd_repair_count_total(void);

/**
 * Zero the persisted repair counter and let the on-screen notice arm again
 * (called after an on-device format).
 */
void sd_repair_reset_total(void);

#ifdef __cplusplus
}
#endif
