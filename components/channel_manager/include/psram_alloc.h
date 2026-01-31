// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef PSRAM_ALLOC_H
#define PSRAM_ALLOC_H

#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SPIRAM-preferring malloc with internal RAM fallback
 */
static inline void *psram_malloc(size_t size) {
    // Try SPIRAM first, fall back to internal RAM
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

/**
 * @brief SPIRAM-preferring calloc with internal RAM fallback
 */
static inline void *psram_calloc(size_t nmemb, size_t size) {
    // Try SPIRAM first with proper calloc (zero-initialized)
    void *p = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
