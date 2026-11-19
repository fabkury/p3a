// SPDX-License-Identifier: Apache-2.0
// The one container format for every binary file p3a keeps on the SD card:
// a 32-byte header followed by `count` fixed-size records.
//
//   channel indices, the local-file scan, playsets, the active playset,
//   pin orders, the blob reference table ... each is a "schema" of this
//   container, not a format of its own.
//
// 1.x had nine formats with nine policies for magic numbers, versions and
// checksums (one had none of the three). Here there is one header, one check,
// and one rule for what a failed check means.
//
// THE RULE. A file that fails its check is never deleted and never silently
// overwritten by the loader. The loader reports why it failed, and the owner
// of the data decides:
//   - a file flagged P3A_FRAME_REBUILDABLE is a cache: its owner rebuilds it
//     (re-lists the channel, re-scans the folder) and saves over it;
//   - a file flagged P3A_FRAME_USER_DATA holds something the user made: its
//     owner must not overwrite it. It reports the problem and leaves the file
//     for a person, or a later firmware, to recover.
//
// Everything is little-endian, which is what the device is. p3a_frame.c is
// pure (host-tested); p3a_frame_file.c reads and writes files.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_FRAME_MAGIC 0x46413350u  // the bytes 'P' '3' 'A' 'F'

// What a container holds. Values are written to disk: never renumber, only
// append. Each schema's owner defines its record and its version.
typedef enum {
    P3A_FRAME_SCHEMA_TEST = 1,  // used by tests only
    P3A_FRAME_SCHEMA_INDEX = 2, // p3a_record_t records in channel order (p3a_index.h)
    // Added by the milestone that brings the owner:
    //   playset, active playset. (Channel indices, the local scan and pin
    //   orders are all P3A_FRAME_SCHEMA_INDEX.)
} p3a_frame_schema_t;

// Flags, written to disk.
#define P3A_FRAME_REBUILDABLE 0x00000001u  // a cache: safe to rebuild and overwrite
#define P3A_FRAME_USER_DATA   0x00000002u  // made by the user: never overwrite on a failed check

typedef struct __attribute__((packed)) {
    uint32_t magic;        // P3A_FRAME_MAGIC
    uint16_t header_size;  // sizeof(p3a_frame_header_t); lets a later header grow
    uint16_t schema;       // p3a_frame_schema_t
    uint16_t version;      // of that schema's record layout
    uint16_t record_size;  // bytes per record
    uint32_t count;        // number of records
    uint32_t flags;
    uint32_t records_crc;  // CRC-32 of the count * record_size bytes after the header
    uint32_t reserved;     // zero
    uint32_t header_crc;   // CRC-32 of the header bytes before this field
} p3a_frame_header_t;

_Static_assert(sizeof(p3a_frame_header_t) == 32, "the frame header is 32 bytes on disk");

// What the caller expects to find.
typedef struct {
    uint16_t schema;
    uint16_t version;
    uint16_t record_size;
    uint32_t max_count;  // refuse a file that claims more records than this
} p3a_frame_expect_t;

// Fills a header for `count` records of `record_size` bytes at `records`.
void p3a_frame_header_make(p3a_frame_header_t *header, uint16_t schema, uint16_t version, uint16_t record_size,
                           uint32_t count, uint32_t flags, const void *records);

// Checks the first bytes of a file as a header, without the records.
//   ESP_ERR_INVALID_SIZE     fewer than 32 bytes
//   P3A_ERR_CORRUPT          bad magic, bad header checksum, or a header size this code does not know
//   ESP_ERR_INVALID_VERSION  intact, but another schema, version or record size than expected:
//                            written by a different firmware, not damaged
//   ESP_ERR_INVALID_SIZE     claims more than expect->max_count records
// On ESP_OK, *records_len (if given) is count * record_size.
esp_err_t p3a_frame_header_check(const void *data, size_t len, const p3a_frame_expect_t *expect, size_t *records_len);

// Checks a whole file image: the header as above, then that all the records
// are present and match their checksum.
//   ESP_ERR_INVALID_SIZE  the records are truncated
//   P3A_ERR_CORRUPT       the records do not match their checksum
// On ESP_OK, *records points into `data` and *count is the number of records.
// Bytes after the last record are ignored.
esp_err_t p3a_frame_check(const void *data, size_t len, const p3a_frame_expect_t *expect, const void **records,
                          uint32_t *count);

// --- files (p3a_frame_file.c) ---------------------------------------------------

// Writes a container atomically (through fs_atomic), creating missing parent
// folders. With P3A_FRAME_USER_DATA the previous file is kept as ".bak" until
// the new one is in place.
esp_err_t p3a_frame_save(const char *path, uint16_t schema, uint16_t version, uint16_t record_size, uint32_t count,
                         uint32_t flags, const void *records);

// Reads and checks a container. On ESP_OK, *records is a buffer the caller
// frees with free() (NULL when count is 0) and *flags (if given) are the
// file's flags.
//   P3A_ERR_MISSING  there is no such file
//   ESP_ERR_NO_MEM   no memory for the records
//   and every result of p3a_frame_check()
// The file is never modified or deleted, whatever the result.
esp_err_t p3a_frame_load(const char *path, const p3a_frame_expect_t *expect, void **records, uint32_t *count,
                         uint32_t *flags);

#ifdef __cplusplus
}
#endif
