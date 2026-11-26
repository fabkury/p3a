// SPDX-License-Identifier: Apache-2.0
// Which release to offer: GitHub's list of a repository's releases, read
// into the one that this device could install. Pure: no IDF, host-tested
// on an answer recorded from GitHub and on damaged copies of it.
//
// A 2.x device installs only an asset named UPDATE_ASSET_NAME. The name is
// the compatibility rule (decisions D12, D85): a release without that asset
// does not exist for this firmware, whatever its number. 1.x units look for
// `p3a.bin` in the same list and so never see a 2.x image; a future break
// (a new partition table, say) ships under a third name and 2.x units will
// not see it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_ASSET_NAME   "p3a-v2.bin"
#define UPDATE_VERSION_LEN  32
#define UPDATE_URL_LEN      256
#define UPDATE_NOTES_LEN    1024
#define UPDATE_DATE_LEN     24
#define UPDATE_MIN_BYTES    (256u * 1024u)  // no firmware is smaller; an asset that is was cut short

typedef struct {
    char version[UPDATE_VERSION_LEN];  // "2.0.1": the tag without its "v"
    char url[UPDATE_URL_LEN];          // where the image is
    uint8_t sha256[32];                // of the image, as GitHub computed it at upload
    uint32_t size;                     // bytes
    bool prerelease;                   // flagged so on GitHub, or a version with a "-suffix"
    char published[UPDATE_DATE_LEN];   // "2026-11-25T18:03:11Z", or empty
    char notes[UPDATE_NOTES_LEN];      // the release's text, cut to fit at a character boundary
} update_release_t;

typedef struct {
    bool prereleases;        // offer pre-releases too
    uint32_t max_bytes;      // the largest image that fits a slot
    const char *url_prefix;  // every download address must start with this
} update_want_t;

// True for "MAJOR.MINOR.PATCH" with an optional "-pre.release" and an
// optional "+build" (semantic versioning 2.0.0), each number at most
// 9 digits, the whole under UPDATE_VERSION_LEN. A leading "v" is not part
// of a version.
bool update_version_ok(const char *version);

// Orders two versions as semantic versioning does: <0, 0, >0. A version
// with a pre-release part comes before the same version without one
// ("2.0.0-dev" < "2.0.0-rc.1" < "2.0.0"); build metadata is ignored. A text
// that is not a version sorts before every version.
int update_version_compare(const char *a, const char *b);

// Reads GitHub's answer to GET /repos/{owner}/{repo}/releases (a JSON array,
// newest first) and gives the highest version this device could install.
// A release is passed over, not an error, when it is a draft, its tag is
// not a version, it is a pre-release and those are not wanted, or it has no
// asset named UPDATE_ASSET_NAME that is whole: uploaded, between
// UPDATE_MIN_BYTES and max_bytes, with a SHA-256 digest, at an address that
// starts with url_prefix and fits.
//
//   ESP_OK             *out is filled. It may be older than what runs: the
//                      caller compares.
//   ESP_ERR_NOT_FOUND  the list is fine and has nothing for this device
//   P3A_ERR_CORRUPT    not the JSON expected
//
// *considered (may be NULL) gets how many releases the list had.
esp_err_t update_release_pick(const char *json, size_t len, const update_want_t *want, update_release_t *out,
                              int *considered);

#ifdef __cplusplus
}
#endif
