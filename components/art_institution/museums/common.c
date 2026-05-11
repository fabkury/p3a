// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/common.c
 * @brief Helpers shared across museum adapters
 *
 * Today this is just a URL encoder; M2 will add Linked-Art walk helpers
 * for Rijksmuseum.
 */

#include "art_institution_internal.h"
#include <stddef.h>

void ai_url_encode(const char *in, char *out, size_t out_len)
{
    static const char *hex = "0123456789ABCDEF";
    if (!in || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 3 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}
