// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Where p3a keeps its files on the SD card. Every path the firmware builds
// goes through here; nothing hard-codes "/sdcard/p3a".
//
// All of p3a's data lives under one root folder. The root is a setting: the
// user may move it (for example to "/art"), and a change takes effect at the
// next boot. The root is fixed for the life of a boot.
//
// sd_path.c is pure (no IDF, host-tested): the root, name sanitizing and the
// shard layout. sd_path_fs.c creates directories and reports to sd_health.
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The root when the user has not chosen one. Includes the mount point.
#define SD_PATH_DEFAULT_ROOT "/sdcard/p3a"

// Longest resolved root, including the terminator.
#define SD_PATH_ROOT_MAX_LEN 64

// --- the root -------------------------------------------------------------------

// Checks a root as the user would write it. Two forms are accepted:
//   "/art"          the usual form; the mount point is added when it is resolved
//   "/sdcard/art"   the full form, used as it is
// It must start with '/', name at least one folder, contain no "..", and fit
// SD_PATH_ROOT_MAX_LEN once resolved.
// ESP_ERR_INVALID_ARG for a malformed path, ESP_ERR_INVALID_SIZE for one too long.
esp_err_t sd_path_validate_root(const char *root);

// Sets the root for this boot from the stored setting. NULL or "" selects the
// default. A stored value that fails sd_path_validate_root() also selects the
// default and returns the validation error, so the caller can say so; the
// module is usable either way. Call once, before anything asks for a path.
esp_err_t sd_path_init(const char *configured_root);

// The resolved root, for example "/sdcard/p3a". The default until sd_path_init().
const char *sd_path_get_root(void);

// "{root}/{subdir}". ESP_ERR_INVALID_SIZE when it does not fit.
esp_err_t sd_path_get_subdir(const char *subdir, char *out_path, size_t out_len);

// The user's own files: "{root}/animations".
esp_err_t sd_path_get_animations(char *out_path, size_t out_len);

// Staging for uploads and downloads in progress: "{root}/temporary".
esp_err_t sd_path_get_temporary(char *out_path, size_t out_len);

// --- names ----------------------------------------------------------------------

// Copies `in` to `out`, replacing each character FAT forbids in a file name
// (: / \ ? * " < > |) with '_'. One byte in, one byte out, and applying it
// twice changes nothing. Truncates silently when `out` is too small; `out`
// may be the same buffer as `in`.
void sd_path_sanitize_filename(const char *in, char *out, size_t out_len);

// --- the shard layout -----------------------------------------------------------
//
// A folder with thousands of files is slow on FAT, so large stores spread
// their files over two levels of 64 folders each:
//
//   {base}/{d0}/{d1}/{sanitized leaf_name}{ext}
//
// d0 and d1 are the low 6 bits of the first two bytes of a 64-bit hash of the
// sanitized leaf name (FNV-1a finished with fmix64), written in decimal, "0"
// to "63". Because the hash is taken over the name as it lands on disk, a
// file's folder can always be worked out again from its name alone. The
// extension is not hashed.
//
// The layout is an on-disk format: changing the hash, the mask or the depth
// re-homes every stored file.
#define SD_SHARD_DEPTH 2
#define SD_SHARD_MASK  0x3F

// Builds a sharded path. `ext` includes its dot, or is "" (NULL is taken as "").
// ESP_ERR_INVALID_ARG for a missing argument or an empty leaf name,
// ESP_ERR_INVALID_SIZE when the result does not fit.
esp_err_t sd_path_build_sharded(const char *base, const char *leaf_name, const char *ext, char *out_path,
                                size_t out_len);

// --- directories (sd_path_fs.c) -------------------------------------------------

// Creates the root and the folders every boot needs (animations, temporary).
// Stores create their own folders on first use through
// sd_path_ensure_parent_dirs().
esp_err_t sd_path_ensure_directories(void);

// Creates every missing folder on the way to `filepath`; the file itself is
// not created. A folder the card has damaged is quarantined and recreated
// (sd_repair). Failures count against the card's health (sd_health), because
// this is the first write most save paths attempt.
esp_err_t sd_path_ensure_parent_dirs(const char *filepath);

#ifdef __cplusplus
}
#endif
