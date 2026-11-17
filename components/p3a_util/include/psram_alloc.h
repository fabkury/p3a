// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file psram_alloc.h
 * @brief SPIRAM-preferring allocator with internal RAM fallback
 */

#ifndef PSRAM_ALLOC_H
#define PSRAM_ALLOC_H

#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// PSRAM buffers that end up in fwrite()/fread() (channel caches, playlists,
// metadata) reach the ESP32-P4 SD host, which DMAs directly to/from PSRAM only
// when the buffer is cache-line aligned; otherwise it bounces 512 bytes per SD
// command (measured: a 15.5 KB cache save took 94 ms and stalled playback,
// jitter work stream RUN-20260829-01). A plain heap_caps_malloc is aligned by
// luck. Align every PSRAM block here; the cost is < 128 bytes per allocation.
// Internal-RAM fallbacks only need 4-byte alignment. Pointers stay free()-able.
#define PSRAM_ALLOC_ALIGN 128

/**
 * @brief SPIRAM-preferring malloc with internal RAM fallback (SPIRAM blocks cache-line aligned)
 */
static inline void *psram_malloc(size_t size) {
    void *p = heap_caps_aligned_alloc(PSRAM_ALLOC_ALIGN, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

/**
 * @brief SPIRAM-preferring calloc with internal RAM fallback (SPIRAM blocks cache-line aligned)
 */
static inline void *psram_calloc(size_t nmemb, size_t size) {
    void *p = heap_caps_aligned_calloc(PSRAM_ALLOC_ALIGN, nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return heap_caps_calloc(nmemb, size, MALLOC_CAP_8BIT);
}

/**
 * @brief SPIRAM-preferring strdup with internal RAM fallback
 */
static inline char *psram_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = (char *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (char *)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    if (p) {
        memcpy(p, s, len);
    }
    return p;
}

// NOTE: psram_realloc is intentionally NOT provided.
// Reallocating between heaps is problematic. If you need realloc,
// use heap_caps_realloc directly with consistent capabilities.

#ifdef __cplusplus
}
#endif

#endif // PSRAM_ALLOC_H
