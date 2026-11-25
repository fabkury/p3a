// SPDX-License-Identifier: Apache-2.0
// "Show this": a picture at an address, fetched and played once.
//
// The picture is saved among the user's own files (the animations folder),
// as an upload is, so it is kept and takes its place in the local channel.
// It is fetched into the temporary folder first and looked at: something
// that is not a picture (a web page, an error served with 200) never
// reaches the user's folder.
//
// The file's name comes from the address's last segment, made safe for the
// card. What the bytes are decides the extension, not the address: an
// address with no extension, or with the wrong one, still gives a file the
// player can open. (1.x refused an address that did not end in a picture's
// extension.)
//
// Each request is a job of its own with its own copy of the address
// (audit M11: 1.x handed the address over through unlocked globals and
// could drop one). Requests are served in the order made.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "p3a_job.h"
#include "p3a_record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_URL_MAX_LEN 1024  // an address, with its terminator

// --- pure (show_url_name.c) -------------------------------------------------------

// Whether this is an address that may be fetched: http:// or https://, a
// host, no spaces or control characters, shorter than SHOW_URL_MAX_LEN.
bool show_url_ok(const char *url);

// The name the picture gets in the user's folder: the address's last path
// segment (percent-escapes resolved, the query left out), with every
// character a card or a record's name cannot hold replaced by '_', and the
// extension of `format` when the name does not already end in one of that
// format's. "shown" when the address has no usable segment.
// ESP_ERR_INVALID_ARG for a format that is not a picture's;
// ESP_ERR_INVALID_SIZE when `len` is too small for any name.
esp_err_t show_url_name(const char *url, p3a_format_t format, char *out, size_t len);

// --- the job (show_url.c) ---------------------------------------------------------

// Queues the fetch and returns at once; *job (if given) identifies it.
// The job ends with:
//   ESP_OK                 saved, and playing unless the device is paused
//   P3A_ERR_NOT_PLAYABLE   what was fetched is not a picture
//   P3A_ERR_MISSING        the address has no such thing (404, 410)
//   P3A_ERR_SD_FAILED      the card cannot be written (damaged, or a computer has it)
//   ESP_ERR_NO_MEM         internal memory is short: not now
// or the fetcher's error. ESP_ERR_INVALID_ARG here, at once, for an address
// that fails show_url_ok().
esp_err_t play_scheduler_show_url(const char *url, p3a_job_id_t *job);

#ifdef __cplusplus
}
#endif
