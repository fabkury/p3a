// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file mem_stats.h
 * @brief Shared memory-usage snapshot helpers.
 *
 * One place to collect a full heap breakdown, with two presentations of the
 * same numbers:
 *   - mem_stats_log()      -> console (ESP_LOGI), human readable
 *   - mem_stats_to_json()  -> cJSON object for the HTTP API
 *
 * Both the on-demand HTTP endpoint (GET /api/memory, see components/http_api)
 * and the periodic auto-report task (see main/p3a_main.c) funnel through these
 * helpers, so the console output is identical regardless of trigger.
 *
 * Particular attention is paid to *internal* RAM, which is the scarce pool on
 * the ESP32-P4 (SPIRAM is plentiful). The INTERNAL|DMA|8BIT mask is also
 * reported because that is the exact allocation the esp_hosted SDIO RX path
 * makes, and its exhaustion is what asserts/panics the chip.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of heap usage at one instant. All sizes are bytes. */
typedef struct {
    int64_t  uptime_us;       ///< esp_timer_get_time() at collection
    unsigned num_tasks;       ///< uxTaskGetNumberOfTasks()

    // Overall default heap (MALLOC_CAP_DEFAULT).
    size_t free_heap;         ///< esp_get_free_heap_size()
    size_t min_free_heap;     ///< esp_get_minimum_free_heap_size() (low-water since boot)
    size_t largest_default;   ///< largest free block, MALLOC_CAP_DEFAULT

    // Internal RAM (MALLOC_CAP_INTERNAL) — the scarce pool we care about most.
    size_t total_internal, free_internal, largest_internal, min_free_internal;

    // External PSRAM (MALLOC_CAP_SPIRAM). total==0 when no PSRAM.
    size_t total_spiram, free_spiram, largest_spiram;

    // DMA-capable (MALLOC_CAP_DMA).
    size_t total_dma, free_dma, largest_dma;

    // Byte-addressable (MALLOC_CAP_8BIT).
    size_t total_8bit, free_8bit, largest_8bit;

    // INTERNAL|DMA|8BIT — the exact mask esp_hosted's SDIO RX path allocates.
    size_t total_sdio_rx, free_sdio_rx, largest_sdio_rx, min_free_sdio_rx;
} mem_stats_t;

/** Fill @p out with a fresh snapshot. Safe from any task context. */
void mem_stats_collect(mem_stats_t *out);

/**
 * @brief Log @p s to the console via ESP_LOGI.
 * @param reason short tag for the report header, e.g. "auto" or "http".
 *               NULL is treated as "report".
 */
void mem_stats_log(const mem_stats_t *s, const char *reason);

/**
 * @brief Build a cJSON object describing @p s.
 * @return newly created object (caller owns / must cJSON_Delete), or NULL on OOM.
 */
cJSON *mem_stats_to_json(const mem_stats_t *s);

#ifdef __cplusplus
}
#endif
