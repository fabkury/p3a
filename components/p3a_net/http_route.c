// SPDX-License-Identifier: Apache-2.0
// See http_route.h. No IDF includes: also compiled by the host tests.

#include "http_route.h"

#include <stdbool.h>
#include <string.h>

// Matches one pattern against one path. Fills params on success.
static bool match_pattern(const char *pattern, const char *path, size_t path_len,
                          p3a_http_param_t params[P3A_HTTP_MAX_PARAMS])
{
    const char *p = pattern;
    const char *s = path;
    const char *end = path + path_len;
    int captured = 0;
    p3a_http_param_t found[P3A_HTTP_MAX_PARAMS] = {{NULL, 0}};

    while (*p != '\0') {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (close == NULL || captured >= P3A_HTTP_MAX_PARAMS) {
                return false;  // a malformed pattern matches nothing
            }
            const char *start = s;
            while (s < end && *s != '/') {
                s++;
            }
            if (s == start) {
                return false;  // an empty segment is not a value
            }
            found[captured].start = start;
            found[captured].len = (size_t)(s - start);
            captured++;
            p = close + 1;
        } else if (*p == '*' && p[1] == '\0' && p > pattern && p[-1] == '/') {
            if (captured >= P3A_HTTP_MAX_PARAMS || s >= end) {
                return false;  // "/static/" alone names nothing
            }
            found[captured].start = s;
            found[captured].len = (size_t)(end - s);
            captured++;
            s = end;
            p++;
        } else {
            if (s >= end || *s != *p) {
                return false;
            }
            s++;
            p++;
        }
    }
    if (s != end) {
        return false;
    }
    if (params != NULL) {
        memcpy(params, found, sizeof(found));
    }
    return true;
}

p3a_http_match_t p3a_http_match(const p3a_http_route_t *routes, size_t count, p3a_http_method_t method,
                                const char *path, size_t path_len, const p3a_http_route_t **route,
                                p3a_http_param_t params[P3A_HTTP_MAX_PARAMS], unsigned *allowed)
{
    unsigned others = 0;
    if (route != NULL) {
        *route = NULL;
    }
    if (routes != NULL && path != NULL) {
        for (size_t i = 0; i < count; i++) {
            p3a_http_param_t found[P3A_HTTP_MAX_PARAMS];
            if (routes[i].pattern == NULL || !match_pattern(routes[i].pattern, path, path_len, found)) {
                continue;
            }
            if (routes[i].method == (uint8_t)method) {
                if (route != NULL) {
                    *route = &routes[i];
                }
                if (params != NULL) {
                    memcpy(params, found, sizeof(found));
                }
                return P3A_HTTP_MATCHED;
            }
            others |= 1u << routes[i].method;
        }
    }
    if (allowed != NULL) {
        *allowed = others;
    }
    return others != 0 ? P3A_HTTP_WRONG_METHOD : P3A_HTTP_NO_SUCH_PATH;
}

const char *p3a_http_method_name(p3a_http_method_t method)
{
    static const char *const names[P3A_HTTP_METHOD_COUNT] = {"GET", "POST", "PUT", "PATCH", "DELETE"};
    return (int)method >= 0 && method < P3A_HTTP_METHOD_COUNT ? names[method] : "?";
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int p3a_http_url_decode(const char *in, size_t in_len, char *buf, size_t buf_len)
{
    if (in == NULL || buf == NULL || buf_len == 0) {
        return -1;
    }
    size_t out = 0;
    for (size_t i = 0; i < in_len; i++) {
        int c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= in_len) {
                return -1;  // an escape cut short by the end of the input
            }
            int hi = hex_digit(in[i + 1]);
            int lo = hex_digit(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            c = (hi << 4) | lo;
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 0x20 || c == 0x7f) {
            return -1;
        }
        if (out + 1 >= buf_len) {
            return -1;
        }
        buf[out++] = (char)c;
    }
    buf[out] = '\0';
    return (int)out;
}
