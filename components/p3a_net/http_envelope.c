// SPDX-License-Identifier: Apache-2.0
// See http_envelope.h. No IDF includes: also compiled by the host tests.

#include "http_envelope.h"

#include <stdio.h>
#include <string.h>

#include "p3a_crc32.h"
#include "p3a_err.h"

char *p3a_http_envelope_ok(cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (data == NULL) {
        data = cJSON_CreateObject();
    }
    if (root == NULL || data == NULL || cJSON_AddBoolToObject(root, "ok", true) == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddItemToObject(root, "data", data);  // root owns data from here
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

// Appends src to buf as the inside of a JSON string, stopping before `limit`.
static size_t append_escaped(char *buf, size_t at, size_t limit, const char *src)
{
    for (; *src != '\0'; src++) {
        unsigned char c = (unsigned char)*src;
        char piece[7];
        size_t n;
        if (c == '"' || c == '\\') {
            piece[0] = '\\';
            piece[1] = (char)c;
            n = 2;
        } else if (c < 0x20 || c == 0x7f) {
            n = (size_t)snprintf(piece, sizeof(piece), "\\u%04x", c);
        } else {
            piece[0] = (char)c;
            n = 1;
        }
        if (at + n > limit) {
            break;
        }
        memcpy(buf + at, piece, n);
        at += n;
    }
    return at;
}

size_t p3a_http_envelope_error(char *buf, size_t buf_len, const char *code, const char *message)
{
    static const char head[] = "{\"ok\":false,\"error\":\"";
    static const char mid[] = "\",\"code\":\"";
    static const char tail[] = "\"}";
    if (buf == NULL || buf_len < P3A_HTTP_ERROR_MIN_BUF) {
        return 0;
    }
    if (code == NULL || code[0] == '\0') {
        code = "ERROR";
    }
    if (message == NULL || message[0] == '\0') {
        message = code;
    }
    // The code is ours and short, but never let it break the envelope: keep
    // only what a code may contain, and at most 32 characters of it.
    char clean[33];
    size_t c = 0;
    for (; code[c] != '\0' && c < sizeof(clean) - 1; c++) {
        char ch = code[c];
        clean[c] = ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') ? ch : '_';
    }
    clean[c] = '\0';

    size_t reserved = sizeof(mid) - 1 + c + sizeof(tail) - 1 + 1;
    size_t at = sizeof(head) - 1;
    memcpy(buf, head, at);
    at = append_escaped(buf, at, buf_len - reserved, message);
    memcpy(buf + at, mid, sizeof(mid) - 1);
    at += sizeof(mid) - 1;
    memcpy(buf + at, clean, c);
    at += c;
    memcpy(buf + at, tail, sizeof(tail));  // with the terminator
    at += sizeof(tail) - 1;
    return at;
}

void p3a_http_error_for(esp_err_t err, int *status, const char **code)
{
    int s;
    const char *c;
    switch (err) {
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        s = 400, c = "INVALID";
        break;
    case ESP_ERR_NOT_FOUND:
        s = 404, c = "NOT_FOUND";
        break;
    case ESP_ERR_INVALID_STATE:
        s = 409, c = "NOT_NOW";
        break;
    case ESP_ERR_NOT_SUPPORTED:
        s = 501, c = "NOT_SUPPORTED";
        break;
    case ESP_ERR_NO_MEM:
        s = 503, c = "NO_MEMORY";
        break;
    case ESP_ERR_TIMEOUT:
        s = 504, c = "TIMEOUT";
        break;
    case P3A_ERR_MISSING:
        s = 404, c = "MISSING";
        break;
    case P3A_ERR_NOT_PLAYABLE:
        s = 422, c = "NOT_PLAYABLE";
        break;
    case P3A_ERR_CORRUPT:
        s = 422, c = "CORRUPT";
        break;
    case P3A_ERR_NO_KEY:
        s = 409, c = "NO_KEY";
        break;
    case P3A_ERR_RATE_LIMITED:
        s = 429, c = "RATE_LIMITED";
        break;
    case P3A_ERR_AUTH:
        s = 502, c = "AUTH";  // a service upstream refused us, not the caller
        break;
    case P3A_ERR_SD_FAILED:
        s = 503, c = "SD_FAILED";
        break;
    default:
        s = 500, c = "INTERNAL";
        break;
    }
    if (status != NULL) {
        *status = s;
    }
    if (code != NULL) {
        *code = c;
    }
}

const char *p3a_http_status_line(int status)
{
    switch (status) {
    case 200:
        return "200 OK";
    case 202:
        return "202 Accepted";
    case 204:
        return "204 No Content";
    case 304:
        return "304 Not Modified";
    case 400:
        return "400 Bad Request";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 409:
        return "409 Conflict";
    case 413:
        return "413 Content Too Large";
    case 414:
        return "414 URI Too Long";
    case 415:
        return "415 Unsupported Media Type";
    case 422:
        return "422 Unprocessable Content";
    case 429:
        return "429 Too Many Requests";
    case 501:
        return "501 Not Implemented";
    case 502:
        return "502 Bad Gateway";
    case 503:
        return "503 Service Unavailable";
    case 504:
        return "504 Gateway Timeout";
    default:
        return "500 Internal Server Error";
    }
}

void p3a_http_etag(const char *body, size_t len, char out[P3A_HTTP_ETAG_LEN])
{
    snprintf(out, P3A_HTTP_ETAG_LEN, "W/\"%08x\"", (unsigned)p3a_crc32(0, body, len));
}

bool p3a_http_etag_matches(const char *if_none_match, const char *etag)
{
    if (if_none_match == NULL || etag == NULL) {
        return false;
    }
    // Weak comparison: the W/ prefix does not matter, the quoted value does.
    const char *want = strncmp(etag, "W/", 2) == 0 ? etag + 2 : etag;
    size_t want_len = strlen(want);
    const char *p = if_none_match;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '*') {
            return true;
        }
        if (strncmp(p, "W/", 2) == 0) {
            p += 2;
        }
        if (want_len > 0 && strncmp(p, want, want_len) == 0) {
            char after = p[want_len];
            if (after == '\0' || after == ',' || after == ' ' || after == '\t') {
                return true;
            }
        }
        while (*p != '\0' && *p != ',') {
            p++;
        }
    }
    return false;
}
