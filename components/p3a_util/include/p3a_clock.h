// SPDX-License-Identifier: Apache-2.0
// Wall-clock time: whether the device knows it, and how to print it.
//
// The device has no battery-backed clock. Until the network layer has
// synchronised it (SNTP), time() counts from 1970 and anything that needs a
// real date (a refresh interval, a TLS certificate check, a timestamp sent to
// a server) must ask p3a_clock_is_set() first.
//
// This lives in util so that any layer can ask; only the network layer calls
// p3a_clock_mark_set().
//
// Host-testable.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called by whoever set the system clock from a trusted source.
void p3a_clock_mark_set(void);

// True once the system clock holds real time.
bool p3a_clock_is_set(void);

// Formats a time as ISO 8601 UTC, "2026-11-18T14:03:22Z". `len` must be at
// least P3A_CLOCK_ISO8601_LEN. ESP_ERR_INVALID_ARG otherwise.
#define P3A_CLOCK_ISO8601_LEN 21
esp_err_t p3a_clock_format_iso8601(time_t when, char *buf, size_t len);

// The current time in that form. ESP_ERR_INVALID_STATE while the clock is not
// set: a 1970 timestamp is worse than none.
esp_err_t p3a_clock_now_iso8601(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
