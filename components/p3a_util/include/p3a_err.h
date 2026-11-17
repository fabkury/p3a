// SPDX-License-Identifier: Apache-2.0
// The p3a error base: the handful of failures the firmware treats differently
// from "something went wrong". They are ordinary esp_err_t values, so they
// travel through any function that returns esp_err_t.
//
// Use one of these when a caller must be able to tell the case apart. Use the
// IDF's own codes (ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT, ...) for everything else.
// Never classify a failure by matching a path or a message string.
//
// Host-testable.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Outside every range the IDF and its components use (they stay below 0x10000,
// except esp_hidd at 0x61000).
#define P3A_ERR_BASE 0x50000

typedef enum {
    // A policy decided not to show this artwork (aspect-ratio filter, decode
    // budget). Nothing is wrong with it: pick again, and do not count a retry,
    // write history, or delete anything.
    P3A_ERR_NOT_PLAYABLE = P3A_ERR_BASE + 1,
    // The bytes are damaged or are not what they claim to be. The only error
    // on which a cached blob may be deleted.
    P3A_ERR_CORRUPT,
    // Something that should exist does not: a blob, a record, a file.
    P3A_ERR_MISSING,
    // A remote service asked us to slow down. Back off; do not retry now.
    P3A_ERR_RATE_LIMITED,
    // A remote service rejected our credentials.
    P3A_ERR_AUTH,
    // The operation needs a key or registration the user has not provided.
    P3A_ERR_NO_KEY,
    // The SD card is latched as failed; no write was attempted.
    P3A_ERR_SD_FAILED,

    P3A_ERR_END_  // one past the last code; not an error
} p3a_err_t;

// True for the codes above.
bool p3a_err_is_p3a(esp_err_t err);

// "NOT_PLAYABLE", "CORRUPT", ... for a p3a code; NULL for anything else.
// These names are stable: logs and the HTTP API use them.
const char *p3a_err_name(esp_err_t err);

// A printable name for any esp_err_t: the p3a name, or esp_err_to_name().
const char *p3a_err_to_name(esp_err_t err);

#ifdef __cplusplus
}
#endif
