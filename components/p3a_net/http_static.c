// SPDX-License-Identifier: Apache-2.0
// See http_static.h. No IDF includes: also compiled by the host tests.

#include "http_static.h"

#include <string.h>

const p3a_static_file_t *p3a_static_find(const char *path, size_t path_len)
{
    static const char index[] = "index.html";
    if (path == NULL || path_len == 0 || path[0] != '/') {
        return NULL;
    }
    bool folder = path[path_len - 1] == '/';
    for (size_t i = 0; i < p3a_static_file_count; i++) {
        const char *have = p3a_static_files[i].path;
        if (have == NULL) {
            continue;
        }
        size_t have_len = strlen(have);
        if (!folder) {
            if (have_len == path_len && memcmp(have, path, path_len) == 0) {
                return &p3a_static_files[i];
            }
        } else if (have_len == path_len + sizeof(index) - 1 && memcmp(have, path, path_len) == 0 &&
                   memcmp(have + path_len, index, sizeof(index) - 1) == 0) {
            return &p3a_static_files[i];
        }
    }
    return NULL;
}
