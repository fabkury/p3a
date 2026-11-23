// SPDX-License-Identifier: Apache-2.0
// The local source: the user's own files in {root}/animations on the SD card.
//
// The files stay where the user put them (they are not moved into the blob
// store), so that someone browsing the card over USB sees their files under
// their names. A local record carries the file's name and the "in place"
// flag, and this source says where it is.
//
// Listing is a scan of the folder: every file with a picture's extension, the
// newest first. Nothing is saved; the folder is the truth and is read again
// at boot and after an upload or a USB session.
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot, after storage is up.
esp_err_t source_local_start(void);

// What the last scan found.
typedef struct {
    uint32_t listed;        // files that became records
    uint32_t not_pictures;  // files passed over: no picture extension, hidden, or temporary
    uint32_t name_too_long; // pictures whose name does not fit a record (over 119 characters)
    uint32_t over_capacity; // pictures beyond what the index holds
    uint32_t ms;            // how long the scan took
} source_local_scan_t;
void source_local_last_scan(source_local_scan_t *out);

// The format a file name promises, by its extension, or P3A_FORMAT_UNKNOWN
// for a file this source passes over. Pure (host-tested). The player still
// reads the real format from the file's bytes.
p3a_format_t source_local_format_of(const char *filename);

#ifdef __cplusplus
}
#endif
