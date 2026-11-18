// SPDX-License-Identifier: Apache-2.0
// Host stand-in for components/p3a_util/include/psram_alloc.h: the same
// names on the C library's allocator, so that the sanitizers see every
// allocation exactly as large as it was asked for.
#pragma once

#include <stdlib.h>
#include <string.h>

static inline void *psram_malloc(size_t size)
{
    return malloc(size);
}

static inline void *psram_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}
