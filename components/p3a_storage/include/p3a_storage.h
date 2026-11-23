// SPDX-License-Identifier: Apache-2.0
// The storage layer's front door: bringing the SD card into service at boot,
// and what p3a assumes about the file system on it.
//
// Layer: storage. Its parts:
//   sd_path     where files live (one root, a runtime setting)
//   fs_atomic   how files are written (temporary file, fsync, rename)
//   sd_health   whether the card can still be trusted (a latch until reboot)
//   sd_repair   undoing damage the card itself did (FAT mirror, lost/)
//   p3a_frame   the one container format for binary files
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- what p3a assumes about the file system --------------------------------------
//
// The card is FAT32, mounted through FatFS. exFAT and anything else is not
// mounted; such a card gets the on-device format flow. Code may rely on the
// following, and should rely on nothing else:
//
//  - Long file names work, up to P3A_FS_NAME_MAX characters. p3a writes ASCII
//    names only: the volume is opened with an OEM code page, and what happens
//    to other characters is not defined.
//  - Names are case-insensitive but case-preserving: "A.GIF" and "a.gif" are
//    the same file. Compare names with strcasecmp.
//  - rename() does not replace an existing file. To replace one, remove it
//    first, which is what fs_atomic does; never rename onto a name directly.
//  - There is no way to fsync a directory. After a power cut a file is either
//    its old self, its new self, or (rarely) missing; fs_atomic's order of
//    operations guarantees it is never a mix.
//  - Modification times have a 2-second resolution and are only as right as
//    the clock was. Before the clock is set from the network they are
//    meaningless, so nothing ages files until then.
//  - Counting free space walks the whole FAT and can take seconds on a large
//    card. Never do it on a task that must stay responsive: use
//    p3a_storage_space(), which returns the last measured figure.
#define P3A_FS_NAME_MAX           255
#define P3A_FS_PATH_MAX           520  // longest path p3a builds, with room for ".tmp" and ".bak"
#define P3A_FS_MTIME_RESOLUTION_S 2

// --- boot ------------------------------------------------------------------------

// Brings the card into service, in the one order that is safe:
//   1. mount it (never formatting);
//   2. compare the two copies of the FAT and repair a damaged sector from the
//      good copy, before anything reads the FAT, then remount if a repair was made;
//   3. set the root folder from `configured_root` (NULL or "" for the default;
//      an invalid value falls back to the default with a warning);
//   4. create the folders every boot needs;
//   5. probe the card: write, rename, read back, delete.
// Then asks a job runner to measure free space.
//
// On failure the card is latched as failed (sd_health) and the device carries
// on without storage. Returns ESP_OK when the card is in service.
esp_err_t p3a_storage_start(const char *configured_root);

// --- the card itself ---------------------------------------------------------------

typedef enum {
    P3A_CARD_IN_SERVICE = 0,  // mounted and passed its probe
    P3A_CARD_FAILED,          // was in service, then stopped accepting writes (latched until restart)
    P3A_CARD_NO_FILESYSTEM,   // a card answers, but p3a cannot mount it: blank, exFAT, damaged
    P3A_CARD_ABSENT,          // nothing answers in the slot
} p3a_card_state_t;

// What the card is to the device now. Instant.
p3a_card_state_t p3a_storage_card_state(void);

// Looks at the slot again, for a card that was absent or unmountable at
// boot: tries to mount it and unmounts it again. Answers what it found;
// P3A_CARD_IN_SERVICE here means "it mounts", and a restart then brings it
// into service. Slow (a mount attempt): call from a job.
p3a_card_state_t p3a_storage_card_probe(void);

// Erases the card and formats it for p3a. Only ever because a person asked
// for it on the device. Nothing may be using the card (stop playback
// first), and the device must be restarted afterwards whatever the result.
// Slow: call from a job.
esp_err_t p3a_storage_format(void);

// --- free space -------------------------------------------------------------------

typedef struct {
    bool valid;              // false until the first measurement finishes
    uint64_t total_bytes;
    uint64_t free_bytes;
    int64_t measured_at_us;  // esp_timer time of the measurement
} p3a_storage_space_t;

// The last measured figure. Instant; never touches the card.
p3a_storage_space_t p3a_storage_space(void);

// Measures now. Slow (see above): call it from a job, after a burst of writes
// or deletions, not from a request handler.
esp_err_t p3a_storage_space_refresh(void);

#ifdef __cplusplus
}
#endif
