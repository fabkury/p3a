// SPDX-License-Identifier: Apache-2.0
// What address may be shown, and the name its picture gets. See show_url.h. Pure.

#include <string.h>

#include "show_url.h"
#include "source_local.h"

bool show_url_ok(const char *url)
{
    if (url == NULL) {
        return false;
    }
    const size_t len = strlen(url);
    if (len >= SHOW_URL_MAX_LEN) {
        return false;
    }
    const char *host = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else {
        return false;
    }
    if (*host == '\0' || *host == '/' || *host == '?' || *host == '#' || *host == ':' || *host == '@') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p != 0; p++) {
        if (*p <= 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

static int hex_value(char c)
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

static const char *extension_of(p3a_format_t format)
{
    switch (format) {
    case P3A_FORMAT_WEBP: return ".webp";
    case P3A_FORMAT_GIF:  return ".gif";
    case P3A_FORMAT_PNG:  return ".png";
    case P3A_FORMAT_JPEG: return ".jpg";
    case P3A_FORMAT_BMP:  return ".bmp";
    default:              return NULL;
    }
}

esp_err_t show_url_name(const char *url, p3a_format_t format, char *out, size_t len)
{
    const char *extension = extension_of(format);
    if (url == NULL || extension == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // A record holds a name of P3A_RECORD_FILENAME_LEN bytes with its terminator.
    const size_t room = len < P3A_RECORD_FILENAME_LEN ? len : P3A_RECORD_FILENAME_LEN;
    if (room < sizeof("shown.webp")) {
        return ESP_ERR_INVALID_SIZE;
    }
    // The path: after the host, up to the query or the fragment.
    const char *path = strstr(url, "://");
    path = path != NULL ? strchr(path + 3, '/') : NULL;
    const char *segment = "";
    size_t segment_len = 0;
    if (path != NULL) {
        const size_t path_len = strcspn(path, "?#");
        // The last segment that is not empty ("/a/b/" names "b").
        size_t end = path_len;
        while (end > 0 && path[end - 1] == '/') {
            end--;
        }
        size_t start = end;
        while (start > 0 && path[start - 1] != '/') {
            start--;
        }
        segment = path + start;
        segment_len = end - start;
    }
    // Escapes resolved, then made safe: letters, digits, '.', '-' and '_'
    // stay; everything else (what FAT forbids, spaces, bytes of UTF-8 that a
    // card's code page may not hold) becomes '_'.
    const size_t stem_room = room - strlen(extension) - 1;
    size_t n = 0;
    for (size_t i = 0; i < segment_len && n < stem_room; i++) {
        unsigned char c = (unsigned char)segment[i];
        if (c == '%' && i + 2 < segment_len && hex_value(segment[i + 1]) >= 0 && hex_value(segment[i + 2]) >= 0) {
            c = (unsigned char)(hex_value(segment[i + 1]) * 16 + hex_value(segment[i + 2]));
            i += 2;
        }
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '-' || c == '_';
        out[n++] = safe ? (char)c : '_';
    }
    out[n] = '\0';
    // A name the local source would pass over as hidden.
    for (size_t i = 0; i < n && out[i] == '.'; i++) {
        out[i] = '_';
    }
    // Nothing but separators and dots is no name.
    if (strspn(out, "_.") == n) {
        strcpy(out, "shown");
        n = strlen(out);
    }
    if (source_local_format_of(out) != format) {
        strcpy(out + n, extension);  // room was kept for it
    }
    return ESP_OK;
}
