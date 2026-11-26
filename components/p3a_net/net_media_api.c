// SPDX-License-Identifier: Apache-2.0
// The artworks' bytes, for the web page's previews (D83).
//
//   GET /api/v2/blobs/{reference}.{ext}   an artwork from the blob store, by the name of its content
//   GET /api/v2/files/{name}              one of the user's own files
//
// The page shows what the device shows by asking the device for it. 1.x
// had the browser fetch each picture from its source, which needed the
// Giphy and Klipy keys in the browser (Klipy's file addresses cannot be
// worked out without asking Klipy) and showed nothing for a file of the
// user's own. A record's JSON names its picture's address here in "file"
// (net_record_json).
//
// A blob's address never changes what it serves (the name is the content's
// hash), so a browser keeps it for good. A file is sent in pieces on the
// server's one task: while a large one goes out, other requests wait.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "net_internal.h"
#include "p3a_blob.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_storage.h"
#include "psram_alloc.h"
#include "sd_path.h"
#include "source_local.h"

#define CHUNK_BYTES 16384

static const char *content_type(p3a_format_t format)
{
    switch (format) {
    case P3A_FORMAT_WEBP: return "image/webp";
    case P3A_FORMAT_GIF:  return "image/gif";
    case P3A_FORMAT_PNG:  return "image/png";  // an APNG is served as one too, and browsers animate it
    case P3A_FORMAT_JPEG: return "image/jpeg";
    case P3A_FORMAT_BMP:  return "image/bmp";
    default:              return "application/octet-stream";
    }
}

// Sends the file at `path`. `etag` is quoted; `forever` for content that
// never changes at its address.
static void send_file(p3a_http_request_t *req, const char *path, p3a_format_t format, const char *etag, bool forever)
{
    httpd_req_t *raw = p3a_http_raw(req);
    char asked[80];
    if (httpd_req_get_hdr_value_str(raw, "If-None-Match", asked, sizeof(asked)) == ESP_OK && strcmp(asked, etag) == 0) {
        httpd_resp_set_status(raw, "304 Not Modified");
        httpd_resp_set_hdr(raw, "ETag", etag);
        httpd_resp_send(raw, NULL, 0);
        p3a_http_mark_answered(req);
        return;
    }
    FILE *f = fopen(path, "rb");
    char *buf = f != NULL ? psram_malloc(CHUNK_BYTES) : NULL;
    if (f == NULL || buf == NULL) {
        if (f != NULL) {
            fclose(f);
            p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the picture");
        } else {
            p3a_http_fail(req, 404, "MISSING", "the device no longer has that picture");
        }
        return;
    }
    httpd_resp_set_type(raw, content_type(format));
    httpd_resp_set_hdr(raw, "ETag", etag);
    httpd_resp_set_hdr(raw, "Cache-Control", forever ? "public, max-age=31536000, immutable" : "no-cache");
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK_BYTES, f)) > 0) {
        err = httpd_resp_send_chunk(raw, buf, (ssize_t)n);
    }
    fclose(f);
    free(buf);
    // The headers have gone: a failure can only cut the transfer short.
    httpd_resp_send_chunk(raw, NULL, 0);
    p3a_http_mark_answered(req);
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void get_blob(p3a_http_request_t *req)
{
    // "{32 hex digits}.{ext}"
    char name[P3A_RECORD_BLOB_REF_LEN * 2 + 8];
    uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
    p3a_format_t format = P3A_FORMAT_UNKNOWN;
    bool ok = p3a_http_param(req, 0, name, sizeof(name)) && strlen(name) > P3A_RECORD_BLOB_REF_LEN * 2 &&
              name[P3A_RECORD_BLOB_REF_LEN * 2] == '.';
    for (int i = 0; ok && i < P3A_RECORD_BLOB_REF_LEN; i++) {
        const int hi = hex_value(name[2 * i]), lo = hex_value(name[2 * i + 1]);
        ok = hi >= 0 && lo >= 0;
        ref[i] = (uint8_t)(hi * 16 + lo);
    }
    for (int f = 1; ok && f < P3A_FORMAT_END_ && format == P3A_FORMAT_UNKNOWN; f++) {
        const char *ext = p3a_blob_extension((p3a_format_t)f);
        if (ext != NULL && strcmp(name + P3A_RECORD_BLOB_REF_LEN * 2, ext) == 0) {
            format = (p3a_format_t)f;
        }
    }
    char path[P3A_FS_PATH_MAX], etag[64];
    if (!ok || format == P3A_FORMAT_UNKNOWN || p3a_blob_path(ref, format, path, sizeof(path)) != ESP_OK) {
        p3a_http_fail(req, 404, "MISSING", "there is no such picture");
        return;
    }
    snprintf(etag, sizeof(etag), "\"%.32s\"", name);
    send_file(req, path, format, etag, true);
}

static void get_file(p3a_http_request_t *req)
{
    char name[P3A_RECORD_FILENAME_LEN + 8], clean[P3A_RECORD_FILENAME_LEN + 8], dir[160], path[P3A_FS_PATH_MAX], etag[64];
    // A name, not a path: what the card could not hold as a name is refused.
    bool ok = p3a_http_param(req, 0, name, sizeof(name)) && strlen(name) < P3A_RECORD_FILENAME_LEN &&
              strchr(name, '/') == NULL && strchr(name, '\\') == NULL && strcmp(name, "..") != 0;
    if (ok) {
        sd_path_sanitize_filename(name, clean, sizeof(clean));
        ok = strcmp(clean, name) == 0;
    }
    const p3a_format_t format = ok ? source_local_format_of(name) : P3A_FORMAT_UNKNOWN;
    struct stat st;
    if (format == P3A_FORMAT_UNKNOWN || sd_path_get_animations(dir, sizeof(dir)) != ESP_OK ||
        snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path) || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        p3a_http_fail(req, 404, "MISSING", "there is no such file among the pictures");
        return;
    }
    // The same name may hold another picture tomorrow.
    snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)st.st_size, (unsigned long)st.st_mtime);
    send_file(req, path, format, etag, false);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/api/v2/blobs/{name}", get_blob},
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/api/v2/files/{name}", get_file},
};

void net_media_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
