// SPDX-License-Identifier: Apache-2.0
// The blob store: the one place artwork bytes are kept on the SD card,
// whatever source they came from.
//
//   {root}/blobs/{d0}/{d1}/{blob_ref as 32 hex digits}.{ext}
//
// A blob is named by its content: blob_ref is the first 16 bytes of the
// SHA-256 of the file. The same picture fetched from two sources, or listed
// in two channels, or pinned, is one file. 1.x had five stores (vault,
// giphy, klipy, museum, pinned), each with its own path builder and
// exists-check, and pinning copied the bytes.
//
// When the card fills up the oldest blobs are deleted (p3a_evict.h). What
// the store does not do yet: sparing the blobs the user pinned. The set of
// kept blobs comes with the pin lists (M4.4) and joins the caller's `keep`.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_BLOB_PATH_MAX 192

// The path of a blob, whether or not it exists. ESP_ERR_INVALID_ARG for an
// all-zero reference (which means "no blob") or an unknown format.
esp_err_t p3a_blob_path(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t format, char *out, size_t out_len);

// Takes a finished file into the store. The file is hashed, named after its
// content and moved into place; if the store already holds those bytes the
// file is deleted instead and *already (if given) is set. Either way the
// file at `path` is gone afterwards and `ref` names the blob. `path` must be
// on the SD card (a move, never a copy): downloads and uploads are written
// under the card's temporary folder.
//   P3A_ERR_MISSING       there is no file at `path`
//   P3A_ERR_CORRUPT       it is empty
//   ESP_ERR_INVALID_ARG   an unknown format
//   ESP_FAIL              it could not be read; other errors from the move
// On failure the file is left where it was.
esp_err_t p3a_blob_adopt(const char *path, p3a_format_t format, uint8_t ref[P3A_RECORD_BLOB_REF_LEN], bool *already);

// The same for a file whose SHA-256 the caller already has (a download
// hashed as it was written: http_fetch's hash_file): the file is not read
// again.
esp_err_t p3a_blob_adopt_hashed(const char *path, p3a_format_t format, const uint8_t sha256[32],
                                uint8_t ref[P3A_RECORD_BLOB_REF_LEN], bool *already);

// Whether the store holds this blob now. One stat(); for picking artworks
// use an index's availability bits, which cost nothing.
bool p3a_blob_exists(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t format);

// Deletes a blob. This is the only code that deletes artwork bytes.
// P3A_ERR_MISSING when it was not there.
esp_err_t p3a_blob_remove(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t format);

// The extension a format's blobs carry, with its dot; NULL for a format this
// firmware does not know. P3A_FORMAT_UNKNOWN has none.
const char *p3a_blob_extension(p3a_format_t format);

#ifdef __cplusplus
}
#endif
