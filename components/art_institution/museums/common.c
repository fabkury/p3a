// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file museums/common.c
 * @brief Helpers shared across museum adapters
 *
 * URL encoder, response-body drainer, and Retry-After parser. The drain
 * and parse helpers were originally duplicated across the AIC, V&A,
 * Wellcome, and SMK adapters; consolidated here so a sixth museum can
 * call them without yet another copy.
 */

#include "art_institution_internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const char *ai_user_agent(void)
{
    // Built once at first use, then reused. Mirrors the format AIC's
    // aic_user_agent() requests on the `AIC-User-Agent` header; sent on
    // the standard `User-Agent` header by both the per-museum search
    // helpers and the shared image-download path. Smithsonian's F5
    // BIG-IP ASM WAF rejects requests with empty/default UA (returns
    // HTTP 200 with a "Request Rejected" HTML body, not a 4xx) — both
    // api.si.edu and ids.si.edu need it. Other museum CDNs accept any
    // reasonable UA, so this is universally safe to send.
    static char s_ua[64];
    static bool s_inited = false;
    if (!s_inited) {
        snprintf(s_ua, sizeof(s_ua), "p3a/%s (pub@kury.dev)", FW_VERSION_STRING);
        s_inited = true;
    }
    return s_ua;
}

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

int ai_drain_body(esp_http_client_handle_t client, char *buf, size_t buf_size)
{
    int total = 0;
    bool read_err = false;
    while (total < (int)buf_size - 1) {
        int n = esp_http_client_read(client, buf + total, buf_size - 1 - total);
        if (n < 0) { read_err = true; break; }
        if (n == 0) break;
        total += n;
    }
    return read_err ? -1 : total;
}

uint32_t ai_parse_retry_after(const char *value)
{
    if (!value) return 0;
    while (*value == ' ') value++;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || v <= 0) return 0;
    if (v > 3600) v = 3600;  // sanity cap
    return (uint32_t)v;
}
