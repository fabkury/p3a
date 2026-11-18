// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file display_renderer_priv.h
 * @brief Display renderer private internals: shared state, macros, upscale workers
 */

#ifndef DISPLAY_RENDERER_PRIV_H
#define DISPLAY_RENDERER_PRIV_H

#include "display_renderer.h"
#include "frame_trace.h"
#include "p3a_board.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include("esp_cache.h")
#include "esp_cache.h"
#define DISPLAY_HAVE_CACHE_MSYNC 1
#else
#define DISPLAY_HAVE_CACHE_MSYNC 0
#endif

#if defined(__XTENSA__)
#include "xtensa/hal.h"
#define DISPLAY_MEMORY_BARRIER() xthal_dcache_sync()
#elif defined(__riscv)
#include "riscv/rv_utils.h"
#define DISPLAY_MEMORY_BARRIER() __asm__ __volatile__ ("fence" ::: "memory")
#else
#define DISPLAY_MEMORY_BARRIER() __asm__ __volatile__ ("" ::: "memory")
#endif

#define DISPLAY_TAG "display_renderer"

#define DISPLAY_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DISPLAY_MAX(a, b) ((a) > (b) ? (a) : (b))

// LCD panel state
extern esp_lcd_panel_handle_t g_display_panel;
extern uint8_t **g_display_buffers;
extern uint8_t g_display_buffer_count;
extern size_t g_display_buffer_bytes;
extern size_t g_display_row_stride;

// Synchronization
extern SemaphoreHandle_t g_display_vsync_sem;
extern TaskHandle_t g_display_producer_task;
extern TaskHandle_t g_display_consumer_task;



// Upscale worker tasks
extern TaskHandle_t g_upscale_worker_top;
extern TaskHandle_t g_upscale_worker_bottom;
extern TaskHandle_t g_upscale_main_task;

// Upscale shared state (set per-frame before notifying workers)
extern const uint8_t *g_upscale_src_buffer;
extern uint8_t *g_upscale_dst_buffer;
extern const uint16_t *g_upscale_lookup_x;
extern const uint16_t *g_upscale_lookup_y;
extern int g_upscale_src_w;
extern int g_upscale_src_h;
extern display_rotation_t g_upscale_rotation;
extern int g_upscale_offset_x;
extern int g_upscale_offset_y;
extern int g_upscale_scaled_w;
extern int g_upscale_scaled_h;
extern volatile bool g_upscale_has_borders;
extern uint8_t g_upscale_bg_r;
extern uint8_t g_upscale_bg_g;
extern uint8_t g_upscale_bg_b;
extern volatile bool g_upscale_worker_top_done;
extern volatile bool g_upscale_worker_bottom_done;

// Multi-buffering state tracking (supports 3+ buffers)
typedef enum {
    BUFFER_STATE_FREE,       // Safe to write
    BUFFER_STATE_RENDERING,  // Being rendered to (producer)
    BUFFER_STATE_READY,      // Producer done; entry sits in g_ready_queue
    BUFFER_STATE_PENDING,    // Submitted, waiting for DMA (consumer)
    BUFFER_STATE_DISPLAYING  // Currently scanned by DMA
} buffer_state_t;

typedef struct {
    volatile buffer_state_t state;
} buffer_info_t;

// Maximum supported buffer count (actual count set via CONFIG_BSP_LCD_DPI_BUFFER_NUMS)
#define P3A_MAX_DISPLAY_BUFFERS 3

extern buffer_info_t g_buffer_info[P3A_MAX_DISPLAY_BUFFERS];
extern volatile int8_t g_displaying_idx;
extern volatile int8_t g_last_submitted_idx;
extern SemaphoreHandle_t g_buffer_free_sem;

// Ready-frame hand-off (producer -> consumer). The producer enqueues a finished
// buffer with its intended on-screen duration and the content generation it was
// rendered for; the consumer dequeues, drops stale-generation frames (see
// display_renderer_note_content_discontinuity), and presents the rest.
typedef struct {
    int8_t   buffer_idx;    // index into g_display_buffers; state == BUFFER_STATE_READY
    uint32_t duration_ms;   // this frame's intended on-screen dwell (from the decoder)
    uint32_t generation;    // content epoch when finalized
    int64_t  produce_end_us; // esp_timer time the producer finished this frame (Phase 6 policy)
#if CONFIG_P3A_FRAME_TRACE
    // Jitter work stream (docs/jitter): producer-side timing carried to the consumer.
    int64_t  ft_produce_start_us;
    int64_t  ft_produce_end_us;
    uint32_t ft_free_wait_us;
    uint32_t ft_decode_us;
    uint32_t ft_upscale_us;
    uint8_t  ft_flags;
#endif
} ready_frame_t;

extern QueueHandle_t g_ready_queue;

// Timing
extern int64_t g_last_frame_present_us;
extern volatile int64_t g_last_vsync_us;


// Internal functions
bool display_panel_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                   esp_lcd_dpi_panel_event_data_t *edata,
                                   void *user_ctx);
void display_producer_task(void *arg);
void display_consumer_task(void *arg);
void display_upscale_worker_top_task(void *arg);
void display_upscale_worker_bottom_task(void *arg);
esp_err_t display_renderer_ensure_upscale_workers(void);

// FPS overlay (display_fps_overlay.c): registers itself as an overlay.
void display_fps_overlay_register(void);

// True while the show_fps setting is on (display_renderer.c follows it).
extern atomic_bool g_display_show_fps;

#endif // DISPLAY_RENDERER_PRIV_H

