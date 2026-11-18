// SPDX-License-Identifier: Apache-2.0
// The one shape of every JSON answer the HTTP API gives:
//
//     {"ok":true,"data":{...}}
//     {"ok":false,"error":"what went wrong, for a person","code":"FOR_A_PROGRAM"}
//
// plus the HTTP status that goes with an error, and the ETag of a body. The
// server applies these in one place; handlers never write an envelope.
//
// Host-testable.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The success envelope around `data`, as text. Takes ownership of `data` in
// every case; NULL data becomes {}. Returns text to release with cJSON_free(),
// or NULL when out of memory.
char *p3a_http_envelope_ok(cJSON *data);

// The error envelope, written into buf with the message escaped for JSON and
// truncated to fit. Needs no heap, so it works when memory has run out.
// Returns the length written, or 0 if buf is smaller than P3A_HTTP_ERROR_MIN_BUF.
#define P3A_HTTP_ERROR_MIN_BUF 96
size_t p3a_http_envelope_error(char *buf, size_t buf_len, const char *code, const char *message);

// The status and code for a failure reported as an esp_err_t, so that the same
// failure reads the same from every endpoint.
void p3a_http_error_for(esp_err_t err, int *status, const char **code);

// "200 OK", "404 Not Found", ... Unknown numbers read as "500 Internal Server Error".
const char *p3a_http_status_line(int status);

// A weak ETag for a body: W/"1a2b3c4d".
#define P3A_HTTP_ETAG_LEN 14
void p3a_http_etag(const char *body, size_t len, char out[P3A_HTTP_ETAG_LEN]);

// True when an If-None-Match header value names this ETag (it may list
// several, or be "*").
bool p3a_http_etag_matches(const char *if_none_match, const char *etag);

#ifdef __cplusplus
}
#endif
