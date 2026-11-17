// SPDX-License-Identifier: Apache-2.0
// Memory pressure on the pool that matters.
//
// The ESP32-P4 has plenty of PSRAM, but one small pool cannot be replaced by
// it: internal, DMA-capable RAM (about 272 KB). The SDIO link to the Wi-Fi
// co-processor, the SD card driver and TLS all allocate from it, and when it
// runs out or fragments, esp_hosted asserts and the SD driver starts failing
// (docs/known-issues.md, "SDIO RX: internal-RAM exhaustion"). 1.x had no way
// to notice; nothing shed load.
//
// This is the signal. Work that can wait (a download, a channel refresh, a
// new TLS session) asks p3a_mem_level() before it starts:
//
//   P3A_MEM_OK        go ahead
//   P3A_MEM_LOW       do not start new optional work; let what runs finish
//   P3A_MEM_CRITICAL  also back off what can be backed off
//
// A change of level is announced as P3A_EVENT_MEM_PRESSURE_CHANGED.
//
// The thresholds are provisional. They are to be set from measurements on the
// 2.0 firmware under a full refresh cycle (docs/2.0/DECISIONS.md, O9).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_MEM_OK = 0,
    P3A_MEM_LOW = 1,
    P3A_MEM_CRITICAL = 2,
} p3a_mem_level_t;

typedef struct {
    // Below either value the level is at least LOW.
    uint32_t low_free_bytes;
    uint32_t low_largest_block;
    // Below either value the level is CRITICAL.
    uint32_t critical_free_bytes;
    uint32_t critical_largest_block;
    // To leave a level, both numbers must clear that level's thresholds by
    // this margin, so that the level does not flap around a threshold.
    uint8_t recover_margin_percent;
} p3a_mem_thresholds_t;

// --- pure logic, host-testable ---

// The provisional thresholds. From the one heap snapshot taken at a 1.x
// crash: 89 percent of the pool in use (about 30 KB free) and a largest free
// block of 8 KB against a 9 KB request.
p3a_mem_thresholds_t p3a_mem_default_thresholds(void);

// The level for one sample, given the level before it. Getting worse takes
// effect at once; getting better needs the recovery margin.
p3a_mem_level_t p3a_mem_evaluate(p3a_mem_level_t previous, uint32_t free_bytes, uint32_t largest_block,
                                 const p3a_mem_thresholds_t *thresholds);

const char *p3a_mem_level_name(p3a_mem_level_t level);

// --- on the device ---

typedef struct {
    uint32_t free_bytes;
    uint32_t largest_block;
    uint32_t min_free_bytes;  // the lowest free_bytes since boot
    uint32_t total_bytes;
    p3a_mem_level_t level;
} p3a_mem_sample_t;

// The current level. Cheap enough to call before every optional operation:
// the pool is sampled at most once per 250 ms and the answer is cached in
// between. Emits P3A_EVENT_MEM_PRESSURE_CHANGED when the level changes.
p3a_mem_level_t p3a_mem_level(void);

// Samples the pool now, bypassing the cache, and returns the numbers.
p3a_mem_sample_t p3a_mem_sample(void);

// Installs the heap report: the first failed allocation of a boot prints one
// snapshot of every heap, with the size, capabilities, function and task of
// the request. One per boot, because failures come in cascades.
void p3a_mem_install_alloc_failure_report(void);

#ifdef __cplusplus
}
#endif
