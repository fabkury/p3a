// SPDX-License-Identifier: Apache-2.0
// The HTTP server: one route table, one matcher, one envelope.
//
// An endpoint is a row in a table of p3a_http_route_t (http_route.h) and a
// handler. The server finds the row, reads the body if the row asks for it,
// calls the handler, and wraps whatever the handler answers:
//
//     static void get_thing(p3a_http_request_t *req)
//     {
//         cJSON *data = cJSON_CreateObject();
//         cJSON_AddNumberToObject(data, "n", 3);
//         p3a_http_ok(req, data);            // {"ok":true,"data":{"n":3}}
//     }
//
// Rules for a handler:
// - Answer exactly once, with one of the p3a_http_ok / accepted / fail calls.
//   (A handler that returns without answering gets a 500 sent for it.)
// - Never write an envelope, a status line or a Content-Type by hand.
// - Never do slow work: anything longer than a few milliseconds is a job
//   (p3a_job_submit) and the answer is p3a_http_accepted() with its id. The
//   handler runs on the server's one task; while it runs, nobody else is
//   served.
// - It runs on an 8 KB stack.
//
// Every GET answered with p3a_http_ok() carries an ETag and honours
// If-None-Match with 304, so a page that polls costs a few bytes when
// nothing changed.
//
// There is no authentication: the API is open to the LAN (decided; see
// docs/2.0-architecture-review/web-ui-and-api.md, section 8).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "http_route.h"
#include "p3a_job.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p3a_http_request p3a_http_request_t;

// Starts the server on port 80 with the built-in /api/v2 routes. Called by
// p3a_net_start(); the server stays up whatever the Wi-Fi state.
esp_err_t p3a_http_start(void);

// Adds a table of routes. The table must stay valid for ever (make it static
// const). Tables are searched in the order they were added, the built-in one
// first. ESP_ERR_NO_MEM when the few slots are taken.
esp_err_t p3a_http_add_routes(const p3a_http_route_t *routes, size_t count);

// --- reading the request --------------------------------------------------------

// Parameter `index` of the route's pattern, URL-decoded into buf. False if
// there is no such parameter, it does not fit, or it is not decodable.
bool p3a_http_param(const p3a_http_request_t *req, int index, char *buf, size_t buf_len);

// The value of a query-string key, URL-decoded. False when absent or unusable.
bool p3a_http_query(const p3a_http_request_t *req, const char *key, char *buf, size_t buf_len);

// The request body, for a route flagged P3A_HTTP_BODY_JSON: always a JSON
// object. Owned by the server.
const cJSON *p3a_http_body(const p3a_http_request_t *req);

// The underlying request, for a route flagged P3A_HTTP_RAW.
httpd_req_t *p3a_http_raw(p3a_http_request_t *req);

// --- answering ------------------------------------------------------------------

// 200 with the success envelope. Takes ownership of data; NULL means {}.
void p3a_http_ok(p3a_http_request_t *req, cJSON *data);

// 202 with {"job": id}: the work continues as a job, readable at
// /api/v2/jobs/{id}.
void p3a_http_accepted(p3a_http_request_t *req, p3a_job_id_t job);

// An error, with a code for programs and a message for people.
void p3a_http_fail(p3a_http_request_t *req, int status, const char *code, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

// An error from an esp_err_t, with the status and code that error always
// gets (http_envelope.h). `what` says what was being attempted.
void p3a_http_fail_err(p3a_http_request_t *req, esp_err_t err, const char *what);

// Call before answering when the answer carries a secret: it is then sent
// with "Cache-Control: no-store" and without an ETag.
void p3a_http_no_store(p3a_http_request_t *req);

// For a P3A_HTTP_RAW handler: tells the server that the handler has sent its
// own response through p3a_http_raw().
void p3a_http_mark_answered(p3a_http_request_t *req);

#ifdef __cplusplus
}
#endif
