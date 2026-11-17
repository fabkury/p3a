// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file fs_atomic.h
 * @brief Shared atomic file-write helper: write to "{final}.tmp", fsync,
 *        rename into place. Single implementation of the pattern previously
 *        duplicated across every SD writer.
 *
 * All variants share these semantics:
 *  - Fail-fast on a latched SD failure: if sd_health_is_failed() and the
 *    destination is under /sdcard, return ESP_ERR_INVALID_STATE without
 *    touching the filesystem (the code callers already treat as "SD not
 *    available, defer").
 *  - Write sequence: unlink stale tmp -> fopen tmp -> write -> fflush ->
 *    fsync -> fclose -> finalize. fsync-before-rename is the FAT power-loss
 *    ordering guarantee; FATFS has no directory fsync, so this is as safe as
 *    FAT gets.
 *  - Finalize (default): unlink(final) then rename(tmp, final) - FATFS
 *    rename won't overwrite an existing destination.
 *  - Finalize (use_bak): rotate final -> "{final}.bak" first and restore it
 *    if the rename into place fails, so the previous version survives a
 *    failed save. .bak is deleted on success.
 *  - Every helper-performed open/write/fsync/rename outcome on an SD path is
 *    reported to sd_health automatically (ENOSPC excluded), so consecutive
 *    failures trip the SD-failure latch.
 */

#pragma once

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool use_bak;  /**< Preserve old final as "{final}.bak"; restore it if the
                        final rename fails. */
} fs_atomic_opts_t;   /**< NULL opts == (fs_atomic_opts_t){ .use_bak = false } */

/**
 * Writer callback for multi-part/streaming payloads. Write the payload to f
 * and return ESP_OK; any other return aborts the save (tmp is unlinked and
 * the writer's error is returned). Writer errors are NOT reported to
 * sd_health (they may be network-side); the helper's own fsync/rename
 * failures still are.
 */
typedef esp_err_t (*fs_atomic_writer_cb_t)(FILE *f, void *ctx);

/** (a) Single buffer -> file, atomically. tmp path is "{final_path}.tmp". */
esp_err_t fs_atomic_write(const char *final_path, const void *data, size_t len,
                          const fs_atomic_opts_t *opts);

/** (b) Streaming variant: opens "{final_path}.tmp", invokes writer, then
 *      fsync + atomic finalize. */
esp_err_t fs_atomic_write_cb(const char *final_path, fs_atomic_writer_cb_t writer,
                             void *ctx, const fs_atomic_opts_t *opts);

/** (c) Finalize an existing, already-written-and-closed tmp file into place. */
esp_err_t fs_atomic_rename(const char *tmp_path, const char *final_path,
                           const fs_atomic_opts_t *opts);

#ifdef __cplusplus
}
#endif
