// SPDX-License-Identifier: Apache-2.0
// The HTTP route table and its one matcher.
//
// A route is a method, a path pattern and a handler. A pattern is a path
// whose segments are either literal or a parameter in braces:
//
//     /api/v2/status
//     /api/v2/jobs/{id}
//     /static/*            a final "*" takes the rest of the path, slashes included
//
// A parameter matches exactly one non-empty segment. Matching is exact
// otherwise: case-sensitive, no trailing-slash forgiveness. The first route
// in table order that matches wins, so a literal route listed before a
// parameter route takes precedence over it.
//
// Host-testable.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_HTTP_GET,
    P3A_HTTP_POST,
    P3A_HTTP_PUT,
    P3A_HTTP_PATCH,
    P3A_HTTP_DELETE,
    P3A_HTTP_METHOD_COUNT
} p3a_http_method_t;

// What the server does before it calls the handler.
#define P3A_HTTP_BODY_JSON 0x01  // read the body and require a JSON object; p3a_http_body() returns it
#define P3A_HTTP_RAW       0x02  // the handler sends its own response (a file, a binary stream)

#define P3A_HTTP_MAX_PARAMS 2

struct p3a_http_request;
typedef void (*p3a_http_handler_t)(struct p3a_http_request *req);

typedef struct {
    uint8_t method;  // p3a_http_method_t
    uint8_t flags;   // P3A_HTTP_*
    const char *pattern;
    p3a_http_handler_t handler;
} p3a_http_route_t;

// A captured parameter: a slice of the path that was matched, not terminated
// and not URL-decoded.
typedef struct {
    const char *start;
    size_t len;
} p3a_http_param_t;

typedef enum {
    P3A_HTTP_MATCHED,
    P3A_HTTP_NO_SUCH_PATH,   // answer 404
    P3A_HTTP_WRONG_METHOD,   // the path exists for other methods: answer 405
} p3a_http_match_t;

// Finds the route for a request. `path` is the URI up to but not including
// any '?'. On P3A_HTTP_MATCHED, *route is set and params[] holds the captures
// in pattern order (unused entries are {NULL, 0}). On P3A_HTTP_WRONG_METHOD,
// *allowed (optional) has bit (1 << method) set for each method the path does
// accept.
p3a_http_match_t p3a_http_match(const p3a_http_route_t *routes, size_t count, p3a_http_method_t method,
                                const char *path, size_t path_len, const p3a_http_route_t **route,
                                p3a_http_param_t params[P3A_HTTP_MAX_PARAMS], unsigned *allowed);

// "GET", "POST", ...; "?" for anything else.
const char *p3a_http_method_name(p3a_http_method_t method);

// Decodes %XX and '+' from a slice of a URI into buf. Returns the decoded
// length, or -1 if it does not fit, contains a malformed escape, or decodes
// to a NUL or control character.
int p3a_http_url_decode(const char *in, size_t in_len, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
