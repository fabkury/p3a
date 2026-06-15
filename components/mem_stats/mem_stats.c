// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "mem_stats.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mem";

// The exact capability mask esp_hosted's SDIO RX path requests in
// sdio_rx_get_buffer(); when that allocation returns NULL the driver asserts
// and the chip panics. This is the value that actually predicts the crash, so
// it is reported as a first-class line. Mirrors HEAP_DIAG_SDIO_RX_CAPS in
// main/p3a_main.c.
#define MEM_SDIO_RX_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

static float pct(size_t used, size_t total)
{
    return total ? (100.0f * (float)used / (float)total) : 0.0f;
}

void mem_stats_collect(mem_stats_t *out)
{
    if (!out) return;

    out->uptime_us = esp_timer_get_time();
    out->num_tasks = (unsigned)uxTaskGetNumberOfTasks();

    out->free_heap       = esp_get_free_heap_size();
    out->min_free_heap   = esp_get_minimum_free_heap_size();
    out->largest_default = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    out->total_internal    = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->free_internal     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->largest_internal  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    out->total_spiram   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->free_spiram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    out->total_dma   = heap_caps_get_total_size(MALLOC_CAP_DMA);
    out->free_dma    = heap_caps_get_free_size(MALLOC_CAP_DMA);
    out->largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    out->total_8bit   = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    out->free_8bit    = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    out->largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    out->total_sdio_rx    = heap_caps_get_total_size(MEM_SDIO_RX_CAPS);
    out->free_sdio_rx     = heap_caps_get_free_size(MEM_SDIO_RX_CAPS);
    out->largest_sdio_rx  = heap_caps_get_largest_free_block(MEM_SDIO_RX_CAPS);
    out->min_free_sdio_rx = heap_caps_get_minimum_free_size(MEM_SDIO_RX_CAPS);
}

void mem_stats_log(const mem_stats_t *s, const char *reason)
{
    if (!s) return;
    if (!reason) reason = "report";

    const size_t used_internal = s->total_internal >= s->free_internal
                                     ? s->total_internal - s->free_internal : 0;
    const size_t used_8bit = s->total_8bit >= s->free_8bit
                                 ? s->total_8bit - s->free_8bit : 0;

    ESP_LOGI(TAG, "=== Memory Status Report (%s) ===", reason);
    ESP_LOGI(TAG, "Uptime: %lld s   FreeRTOS tasks: %u",
             (long long)(s->uptime_us / 1000000), s->num_tasks);

    // Scan-friendly one-liner: the value to watch for SDIO RX assert prediction.
    ESP_LOGI(TAG, "heap[INT+DMA+8BIT] free=%zu min=%zu largest=%zu / total=%zu  (SDIO RX alloc mask)",
             s->free_sdio_rx, s->min_free_sdio_rx, s->largest_sdio_rx, s->total_sdio_rx);

    ESP_LOGI(TAG, "Overall Heap: free=%zu (%.2f KB)  min_free=%zu (%.2f KB)  largest=%zu (%.2f KB)",
             s->free_heap, s->free_heap / 1024.0f,
             s->min_free_heap, s->min_free_heap / 1024.0f,
             s->largest_default, s->largest_default / 1024.0f);

    ESP_LOGI(TAG, "Internal RAM: total=%zu (%.2f KB) used=%zu (%.1f%%) free=%zu (%.2f KB) min_free=%zu largest=%zu",
             s->total_internal, s->total_internal / 1024.0f,
             used_internal, pct(used_internal, s->total_internal),
             s->free_internal, s->free_internal / 1024.0f,
             s->min_free_internal, s->largest_internal);

    if (s->total_spiram > 0) {
        const size_t used_spiram = s->total_spiram >= s->free_spiram
                                       ? s->total_spiram - s->free_spiram : 0;
        ESP_LOGI(TAG, "SPIRAM: total=%zu (%.2f KB) used=%zu (%.1f%%) free=%zu (%.2f KB) largest=%zu",
                 s->total_spiram, s->total_spiram / 1024.0f,
                 used_spiram, pct(used_spiram, s->total_spiram),
                 s->free_spiram, s->free_spiram / 1024.0f, s->largest_spiram);
    }

    if (s->total_dma > 0) {
        const size_t used_dma = s->total_dma >= s->free_dma
                                    ? s->total_dma - s->free_dma : 0;
        ESP_LOGI(TAG, "DMA-capable: total=%zu (%.2f KB) used=%zu (%.1f%%) free=%zu (%.2f KB) largest=%zu",
                 s->total_dma, s->total_dma / 1024.0f,
                 used_dma, pct(used_dma, s->total_dma),
                 s->free_dma, s->free_dma / 1024.0f, s->largest_dma);
    }

    ESP_LOGI(TAG, "8-bit accessible: total=%zu (%.2f KB) used=%zu (%.1f%%) free=%zu (%.2f KB) largest=%zu",
             s->total_8bit, s->total_8bit / 1024.0f,
             used_8bit, pct(used_8bit, s->total_8bit),
             s->free_8bit, s->free_8bit / 1024.0f, s->largest_8bit);

    ESP_LOGI(TAG, "================================");
}

// Add a {total,free,used,used_pct,largest_free_block} sub-object under parent.
static void add_cap(cJSON *parent, const char *name,
                    size_t total, size_t free_bytes, size_t largest)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    const size_t used = total >= free_bytes ? total - free_bytes : 0;
    cJSON_AddNumberToObject(o, "total", (double)total);
    cJSON_AddNumberToObject(o, "free", (double)free_bytes);
    cJSON_AddNumberToObject(o, "used", (double)used);
    cJSON_AddNumberToObject(o, "used_pct", total ? (100.0 * (double)used / (double)total) : 0.0);
    cJSON_AddNumberToObject(o, "largest_free_block", (double)largest);
    cJSON_AddItemToObject(parent, name, o);
}

cJSON *mem_stats_to_json(const mem_stats_t *s)
{
    if (!s) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "uptime_sec", (double)(s->uptime_us / 1000000));
    cJSON_AddNumberToObject(root, "tasks", (double)s->num_tasks);

    cJSON *heap = cJSON_CreateObject();
    if (heap) {
        cJSON_AddNumberToObject(heap, "free", (double)s->free_heap);
        cJSON_AddNumberToObject(heap, "min_free", (double)s->min_free_heap);
        cJSON_AddNumberToObject(heap, "largest_free_block", (double)s->largest_default);
        cJSON_AddItemToObject(root, "heap", heap);
    }

    add_cap(root, "internal", s->total_internal, s->free_internal, s->largest_internal);
    cJSON *internal = cJSON_GetObjectItem(root, "internal");
    if (internal) {
        cJSON_AddNumberToObject(internal, "min_free", (double)s->min_free_internal);
    }

    if (s->total_spiram > 0) {
        add_cap(root, "spiram", s->total_spiram, s->free_spiram, s->largest_spiram);
    }
    if (s->total_dma > 0) {
        add_cap(root, "dma", s->total_dma, s->free_dma, s->largest_dma);
    }
    add_cap(root, "all_8bit", s->total_8bit, s->free_8bit, s->largest_8bit);

    add_cap(root, "sdio_rx", s->total_sdio_rx, s->free_sdio_rx, s->largest_sdio_rx);
    cJSON *sdio = cJSON_GetObjectItem(root, "sdio_rx");
    if (sdio) {
        cJSON_AddNumberToObject(sdio, "min_free", (double)s->min_free_sdio_rx);
        cJSON_AddStringToObject(sdio, "caps", "INTERNAL|DMA|8BIT");
    }

    return root;
}
