// SPDX-License-Identifier: Apache-2.0
// Web files built into the firmware. tools/embed_static.py turns the folder
// components/p3a_net/static/ into the table below at build time: to add a
// page, add a file there. A file at static/setup/index.html is served at
// /setup/index.html, gzipped, with an ETag.
//
// Host-testable.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;          // "/setup/index.html"
    const unsigned char *data;
    size_t len;
    const char *content_type;
    const char *etag;          // quoted, strong: "1a2b3c4d"
    bool gzipped;
} p3a_static_file_t;

extern const p3a_static_file_t p3a_static_files[];
extern const size_t p3a_static_file_count;

// The file served at a path, or NULL. `path` need not be terminated at
// path_len. A path that ends in '/' finds the index.html of that folder.
const p3a_static_file_t *p3a_static_find(const char *path, size_t path_len);

#ifdef __cplusplus
}
#endif
