// SPDX-License-Identifier: Apache-2.0
// The record: what p3a knows about one artwork, whichever source it came
// from. 192 bytes, the same layout in memory and on the SD card.
//
// 1.x had five 64-byte layouts that shared their first bytes by convention,
// with nothing in the record saying which one it was; identifiers that did
// not fit were skipped or cut. Here the source is a field, and the identity
// part is sized for the longest real identifier (a 95-character IIIF key).
//
// Pure: no IDF includes, host-tested (tests/host/library/test_record.c).
// Everything is little-endian, which is what the device is.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P3A_RECORD_SIZE    192
#define P3A_RECORD_VERSION 1  // of this layout, as written in a container's header

// Where an artwork comes from. Written to disk: never renumber, only append.
typedef enum {
    P3A_SOURCE_NONE = 0,  // not a record
    P3A_SOURCE_LOCAL = 1,
    P3A_SOURCE_MAKAPIX = 2,
    P3A_SOURCE_GIPHY = 3,
    P3A_SOURCE_KLIPY = 4,
    P3A_SOURCE_MUSEUM = 5,
    P3A_SOURCE_END_  // one past the last source this firmware knows
} p3a_source_t;

// The file format, when known. Written to disk.
typedef enum {
    P3A_FORMAT_UNKNOWN = 0,  // not downloaded or not looked at yet
    P3A_FORMAT_WEBP = 1,
    P3A_FORMAT_GIF = 2,
    P3A_FORMAT_PNG = 3,
    P3A_FORMAT_JPEG = 4,
    P3A_FORMAT_BMP = 5,
    P3A_FORMAT_END_
} p3a_format_t;

// Flags, written to disk.
#define P3A_RECORD_UNRESOLVED 0x01  // the source has not yet said where the bytes are (museum renditions)
#define P3A_RECORD_GONE       0x02  // the source says it no longer exists; kept so that it is not listed again

// Sizes of the identifiers, terminator included.
#define P3A_RECORD_GIPHY_ID_LEN  32
#define P3A_RECORD_IIIF_KEY_LEN  96
#define P3A_RECORD_FILENAME_LEN  120
#define P3A_RECORD_BLOB_REF_LEN  16

typedef struct __attribute__((packed)) {
    // --- common part, 64 bytes ---
    uint8_t source;          // p3a_source_t: says which member of `identity` is in use
    uint8_t kind;            // 0: an artwork. Other values are reserved
    uint8_t format;          // p3a_format_t
    uint8_t flags;           // P3A_RECORD_*
    uint32_t id32;           // the record's key within its index: a server's own number
                             // (Makapix post id) or p3a_record_hash() of the identity
    uint32_t sort_at;        // what the channel is ordered by, Unix seconds
    uint32_t modified_at;    // when the artwork last changed at the source, Unix seconds
    uint16_t width, height;  // in pixels; 0 until the player has decoded it once
    uint8_t resolve_fails;   // consecutive failures to find where the bytes are
    uint8_t download_fails;  // consecutive failures to fetch them
    uint8_t blob_ref[P3A_RECORD_BLOB_REF_LEN];  // the content's name in the blob store; all zero until downloaded
    uint8_t reserved[26];    // zero

    // --- identity, 128 bytes: what the source calls this artwork ---
    union {
        struct __attribute__((packed)) {
            uint8_t uuid[16];  // the storage key
        } makapix;
        struct __attribute__((packed)) {
            char id[P3A_RECORD_GIPHY_ID_LEN];
        } giphy;
        struct __attribute__((packed)) {
            uint64_t id;
            uint8_t product;  // GIF or sticker, as the Klipy source numbers them
        } klipy;
        struct __attribute__((packed)) {
            uint16_t museum_id;
            char iiif_key[P3A_RECORD_IIIF_KEY_LEN];
        } museum;
        struct __attribute__((packed)) {
            char filename[P3A_RECORD_FILENAME_LEN];
        } local;
        uint8_t raw[128];
    } identity;
} p3a_record_t;

_Static_assert(sizeof(p3a_record_t) == P3A_RECORD_SIZE, "a record is 192 bytes on disk");
_Static_assert(offsetof(p3a_record_t, identity) == 64, "the identity starts at byte 64");

// --- making records --------------------------------------------------------------
//
// Each fills a zeroed record with its source, identity and id32. An
// identifier that does not fit is refused (ESP_ERR_INVALID_SIZE), never cut:
// a cut identifier names a different artwork, or none.

esp_err_t p3a_record_init_local(p3a_record_t *record, const char *filename);
esp_err_t p3a_record_init_makapix(p3a_record_t *record, uint32_t post_id, const uint8_t uuid[16]);
esp_err_t p3a_record_init_giphy(p3a_record_t *record, const char *id);
esp_err_t p3a_record_init_klipy(p3a_record_t *record, uint64_t id, uint8_t product);
esp_err_t p3a_record_init_museum(p3a_record_t *record, uint16_t museum_id, const char *iiif_key);

// --- reading records -------------------------------------------------------------

// Checks a record that came from a file: a source this firmware knows, text
// identifiers terminated inside their fields, known format, reserved bytes
// zero. ESP_OK or P3A_ERR_CORRUPT.
esp_err_t p3a_record_check(const p3a_record_t *record);

// True when both records name the same artwork at the same source, whatever
// else differs. Two artworks may share an id32 (it is a 32-bit hash for most
// sources); this is what tells them apart.
bool p3a_record_same_identity(const p3a_record_t *a, const p3a_record_t *b);

// True once the artwork's bytes are in the blob store.
bool p3a_record_has_blob(const p3a_record_t *record);

// The id32 of an identity that has no number of its own: FNV-1a over the
// source and the bytes. Never 0.
uint32_t p3a_record_hash(p3a_source_t source, const void *bytes, size_t len);

// A source's name for logs and the API ("local", "makapix", ...); "unknown"
// for a number this firmware does not know.
const char *p3a_source_name(uint8_t source);

#ifdef __cplusplus
}
#endif
