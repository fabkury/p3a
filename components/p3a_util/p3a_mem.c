// SPDX-License-Identifier: Apache-2.0
// The device half of p3a_mem.h: sampling the pool and the failure report.
#include "p3a_mem.h"

#include <stdatomic.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_event.h"

// The pool esp_hosted's SDIO receive path, the SD driver and TLS share.
#define POOL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define SAMPLE_INTERVAL_US (250 * 1000)

static const char *TAG = "p3a_mem";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static p3a_mem_level_t s_level = P3A_MEM_OK;
static int64_t s_sampled_at_us = -SAMPLE_INTERVAL_US;

p3a_mem_sample_t p3a_mem_sample(void)
{
    // Walking the heap for its largest block is the costly part; it happens
    // outside the spinlock, which only guards the cached level.
    p3a_mem_sample_t sample = {
        .free_bytes = (uint32_t)heap_caps_get_free_size(POOL_CAPS),
        .largest_block = (uint32_t)heap_caps_get_largest_free_block(POOL_CAPS),
        .min_free_bytes = (uint32_t)heap_caps_get_minimum_free_size(POOL_CAPS),
        .total_bytes = (uint32_t)heap_caps_get_total_size(POOL_CAPS),
    };
    const p3a_mem_thresholds_t thresholds = p3a_mem_default_thresholds();

    taskENTER_CRITICAL(&s_mux);
    const p3a_mem_level_t before = s_level;
    sample.level = p3a_mem_evaluate(before, sample.free_bytes, sample.largest_block, &thresholds);
    s_level = sample.level;
    s_sampled_at_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);

    if (sample.level != before) {
        ESP_LOGW(TAG, "DMA pool pressure %s -> %s (free %u, largest block %u of %u)", p3a_mem_level_name(before),
                 p3a_mem_level_name(sample.level), (unsigned)sample.free_bytes, (unsigned)sample.largest_block,
                 (unsigned)sample.total_bytes);
        p3a_event_emit_i32(P3A_EVENT_MEM_PRESSURE_CHANGED, (int32_t)sample.level);
    }
    return sample;
}

p3a_mem_level_t p3a_mem_level(void)
{
    taskENTER_CRITICAL(&s_mux);
    const p3a_mem_level_t level = s_level;
    const bool fresh = esp_timer_get_time() - s_sampled_at_us < SAMPLE_INTERVAL_US;
    taskEXIT_CRITICAL(&s_mux);
    return fresh ? level : p3a_mem_sample().level;
}

// --- the allocation-failure report --------------------------------------------

static atomic_flag s_reported = ATOMIC_FLAG_INIT;

static void print_heap(const char *label, uint32_t caps)
{
    esp_rom_printf("  %s free=%u largest=%u total=%u\n", label, (unsigned)heap_caps_get_free_size(caps),
                   (unsigned)heap_caps_get_largest_free_block(caps), (unsigned)heap_caps_get_total_size(caps));
}

// Runs in the context of the failed allocation: any task, an interrupt, or a
// critical section. esp_rom_printf is safe in all of them, and the heap lock
// is not held when the callback runs, so the heap can be queried.
static void alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    if (atomic_flag_test_and_set(&s_reported)) {
        return;
    }
    const char *task = xPortInIsrContext() ? "ISR" : pcTaskGetName(NULL);
    esp_rom_printf("\n*** HEAP ALLOC FAILED (one snapshot per boot) ***\n");
    esp_rom_printf("  func=%s size=%u caps=0x%08x task=%s\n", function_name ? function_name : "?", (unsigned)size,
                   (unsigned)caps, task ? task : "?");
    print_heap("INTERNAL+DMA (SDIO, SD, TLS):", POOL_CAPS);
    print_heap("INTERNAL:                    ", MALLOC_CAP_INTERNAL);
    print_heap("DMA:                         ", MALLOC_CAP_DMA);
    print_heap("SPIRAM:                      ", MALLOC_CAP_SPIRAM);
    print_heap("DEFAULT:                     ", MALLOC_CAP_DEFAULT);
    esp_rom_printf("************************************************\n\n");
}

void p3a_mem_install_alloc_failure_report(void)
{
    heap_caps_register_failed_alloc_callback(alloc_failed);
}
