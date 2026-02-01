// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef DISPLAY_RENDERER_PRIV_H
#define DISPLAY_RENDERER_PRIV_H

#include "display_renderer.h"
#include "p3a_board.h"
#include "app_lcd.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
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
extern SemaphoreHandle_t g_display_mutex;
extern TaskHandle_t g_display_render_task;

// Render mode
extern volatile display_render_mode_t g_display_mode_request;
extern volatile display_render_mode_t g_display_mode_active;

// Frame callback
extern display_frame_callback_t g_display_frame_callback;
extern void *g_display_frame_callback_ctx;

// Upscale worker tasks
extern TaskHandle_t g_upscale_worker_top;
extern TaskHandle_t g_upscale_worker_bottom;
extern TaskHandle_t g_upscale_main_task;

// Upscale shared state (set per-frame before notifying workers)
extern const uint8_t *g_upscale_src_buffer;
extern int g_upscale_src_bpp;
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
extern uint16_t g_upscale_bg_rgb565;
extern int g_upscale_row_start_top;
extern int g_upscale_row_end_top;
extern int g_upscale_row_start_bottom;
extern int g_upscale_row_end_bottom;
extern volatile bool g_upscale_worker_top_done;
extern volatile bool g_upscale_worker_bottom_done;

// Buffer management
extern uint8_t g_render_buffer_index;
extern uint8_t g_last_display_buffer;

// Multi-buffering state tracking (supports 3+ buffers)
typedef enum {
    BUFFER_STATE_FREE,       // Safe to write
    BUFFER_STATE_RENDERING,  // Being rendered to
    BUFFER_STATE_PENDING,    // Submitted, waiting for DMA
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

// Timing
extern int64_t g_last_frame_present_us;
extern uint32_t g_target_frame_delay_ms;

// Screen rotation
extern display_rotation_t g_screen_rotation;
extern volatile bool g_rotation_in_progress;

// Internal functions
bool display_panel_refresh_done_cb(esp_lcd_panel_handle_t panel, 
                                   esp_lcd_dpi_panel_event_data_t *edata, 
                                   void *user_ctx);
void display_render_task(void *arg);
void display_upscale_worker_top_task(void *arg);
void display_upscale_worker_bottom_task(void *arg);
esp_err_t display_renderer_ensure_upscale_workers(void);

// FPS overlay (display_fps_overlay.c)
void fps_update_and_draw(uint8_t *buffer);

// ============================================================================
// Processing Notification (display_processing_notification.c)
// ============================================================================

/**
 * @brief Processing notification state
 */
typedef enum {
    PROC_NOTIF_STATE_IDLE,       ///< Not showing
    PROC_NOTIF_STATE_PROCESSING, ///< Blue triangle - swap in progress
    PROC_NOTIF_STATE_FAILED      ///< Red triangle - swap failed, showing for 3 seconds
} proc_notif_state_t;

// State variables (defined in display_renderer.c)
extern volatile proc_notif_state_t g_proc_notif_state;
extern volatile int64_t g_proc_notif_start_time_us;
extern volatile int64_t g_proc_notif_fail_time_us;

/**
 * @brief Start processing notification (call when user initiates swap)
 */
void proc_notif_start(void);

/**
 * @brief Signal successful swap (clears notification immediately)
 */
void proc_notif_success(void);

/**
 * @brief Update and draw processing notification overlay
 * 
 * Called each frame from display_render_task. Handles state machine
 * transitions and draws the triangle if active.
 * 
 * @param buffer Frame buffer to draw into
 */
void processing_notification_update_and_draw(uint8_t *buffer);

// RGB565 conversion helper (shared between files)
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

#endif // DISPLAY_RENDERER_PRIV_H

