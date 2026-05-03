// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

// Override ESP-IDF's default ff_memalloc/ff_memfree (fatfs/port/freertos/ffsystem.c)
// so FATFS hands the SDMMC driver buffers it can DMA from directly.
//
// The default allocator returns PSRAM via heap_caps_malloc_prefer() but does
// not request cache-line alignment. On ESP32-P4 the SDMMC driver's
// sdmmc_host_check_buffer_alignment() rejects PSRAM buffers that aren't 64-byte
// aligned and falls back to allocating a 4 KB MALLOC_CAP_DMA bounce buffer per
// transfer (sdmmc/sdmmc_cmd.c:584-628). After ~30 s of churn through a 64-channel
// playset the internal RAM heap fragments enough that the bounce alloc fails
// with ESP_ERR_NO_MEM (0x101), cascading into "sdmmc_read_sectors: not enough
// mem" floods and EIO from every subsequent SD op.
//
// Forcing MALLOC_CAP_CACHE_ALIGNED removes the bounce path: aligned PSRAM
// buffers go to DMA directly, no internal-RAM dependency per SD I/O.
//
// Wired in via `-Wl,--wrap=ff_memalloc -Wl,--wrap=ff_memfree` in main/CMakeLists.txt;
// the linker rewrites cross-TU calls from ff.c / vfs_fat*.c to land here.

#include "esp_heap_caps.h"

void *__wrap_ff_memalloc(unsigned msize)
{
    void *p = heap_caps_malloc(msize,
        MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(msize,
            MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

void __wrap_ff_memfree(void *mblock)
{
    heap_caps_free(mblock);
}
