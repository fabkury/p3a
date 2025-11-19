/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "animation_player.h"
#include "animation_decoder.h"
#include "app_lcd.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include("esp_cache.h")
#include "esp_cache.h"
#define APP_LCD_HAVE_CACHE_MSYNC 1
#else
#define APP_LCD_HAVE_CACHE_MSYNC 0
#endif

#include "bsp/esp-bsp.h"

// For memory barriers to ensure cache coherency between cores
#if defined(__XTENSA__)
#include "xtensa/hal.h"
#define MEMORY_BARRIER() xthal_dcache_sync()
#elif defined(__riscv)
#include "riscv/rv_utils.h"
#define MEMORY_BARRIER() __asm__ __volatile__ ("fence" ::: "memory")
#else
#define MEMORY_BARRIER() __asm__ __volatile__ ("" ::: "memory")
#endif

#define TAG "anim_player"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define PICO8_FRAME_WIDTH        128
#define PICO8_FRAME_HEIGHT       128
#define PICO8_PALETTE_COLORS     16
#define PICO8_FRAME_BYTES        (PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT / 2)
#define PICO8_STREAM_TIMEOUT_US  (250 * 1000)

#define DIGIT_WIDTH  5
#define DIGIT_HEIGHT 7

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pico8_color_t;

// Asset file type
typedef enum {
    ASSET_TYPE_WEBP,
    ASSET_TYPE_GIF,
    ASSET_TYPE_PNG,
    ASSET_TYPE_JPEG,
} asset_type_t;

// SD card animation file list
typedef struct {
    char **filenames;
    asset_type_t *types;
    bool *health_flags;  // true = file is ok, false = file has issues
    size_t count;
    size_t current_index;
    char *animations_dir;
} app_lcd_sd_file_list_t;

// Animation buffer structure - encapsulates all state for one animation
typedef struct {
    animation_decoder_t *decoder;
    const uint8_t *file_data;
    size_t file_size;
    animation_decoder_info_t decoder_info;
    asset_type_t type;
    size_t asset_index;
    
    // Native frame buffers (B1/B2 for double buffering during decode)
    uint8_t *native_frame_b1;
    uint8_t *native_frame_b2;
    uint8_t native_buffer_active;
    size_t native_frame_size;
    
    // Upscale lookup tables
    uint16_t *upscale_lookup_x;
    uint16_t *upscale_lookup_y;
    int upscale_src_w, upscale_src_h;
    int upscale_dst_w, upscale_dst_h;
    
    // Prefetched first frame (LCD-sized, already upscaled)
    uint8_t *prefetched_first_frame;
    bool first_frame_ready;
    bool decoder_at_frame_1;  // True if decoder has advanced past frame 0
    bool prefetch_pending;    // True if prefetch needs to be done (by render task)
    uint32_t prefetched_first_frame_delay_ms;  // Delay for the prefetched first frame
    uint32_t current_frame_delay_ms;  // Delay for the most recently decoded frame
    
    bool ready;  // True when fully loaded and ready to play
} animation_buffer_t;

// Player state
static esp_lcd_panel_handle_t s_display_handle = NULL;
static uint8_t **s_lcd_buffers = NULL;
static uint8_t s_buffer_count = 0;
static size_t s_frame_buffer_bytes = 0;
static size_t s_frame_row_stride_bytes = 0;

static SemaphoreHandle_t s_vsync_sem = NULL;
static TaskHandle_t s_anim_task = NULL;

// Double buffer system
static animation_buffer_t s_front_buffer = {0};  // Currently playing animation
static animation_buffer_t s_back_buffer = {0};   // Next animation (preloaded)
static size_t s_next_asset_index = 0;             // Index of animation to load into back buffer
static bool s_swap_requested = false;            // Flag to request buffer swap
static bool s_loader_busy = false;               // Flag to prevent duplicate loader triggers
static TaskHandle_t s_loader_task = NULL;        // Background loader task handle
static SemaphoreHandle_t s_loader_sem = NULL;    // Semaphore to signal loader task
static SemaphoreHandle_t s_buffer_mutex = NULL;  // Mutex for buffer synchronization

static bool s_anim_paused = false;

// Parallel upscaling workers - use buffer-specific lookup tables
static TaskHandle_t s_upscale_worker_top = NULL;
static TaskHandle_t s_upscale_worker_bottom = NULL;
static TaskHandle_t s_upscale_main_task = NULL;
static const uint8_t *s_upscale_src_buffer = NULL;
static uint8_t *s_upscale_dst_buffer = NULL;
static const uint16_t *s_upscale_lookup_x = NULL;
static const uint16_t *s_upscale_lookup_y = NULL;
static int s_upscale_src_w = 0;
static int s_upscale_src_h = 0;
static int s_upscale_row_start_top = 0;
static int s_upscale_row_end_top = 0;
static int s_upscale_row_start_bottom = 0;
static int s_upscale_row_end_bottom = 0;
static volatile bool s_upscale_worker_top_done = false;
static volatile bool s_upscale_worker_bottom_done = false;

static uint8_t s_render_buffer_index = 0;
static uint8_t s_last_display_buffer = 0;

static int64_t s_last_frame_present_us = 0;
static int64_t s_last_duration_update_us = 0;
static int s_latest_frame_duration_ms = 0;
static char s_frame_duration_text[11] = "";
static int64_t s_frame_processing_start_us = 0;  // When current frame processing started
static uint32_t s_target_frame_delay_ms = 16;     // Target delay for current frame

static app_lcd_sd_file_list_t s_sd_file_list = {0};
static bool s_sd_mounted = false;
static bool s_sd_export_active = false;

static uint8_t *s_pico8_frame_buffers[2] = { NULL, NULL };
static uint8_t s_pico8_decode_index = 0;
static uint8_t s_pico8_display_index = 0;
static bool s_pico8_frame_ready = false;
static bool s_pico8_override_active = false;
static int64_t s_pico8_last_frame_time_us = 0;
static uint16_t *s_pico8_lookup_x = NULL;
static uint16_t *s_pico8_lookup_y = NULL;
static bool s_pico8_palette_initialized = false;

static const pico8_color_t s_pico8_palette_defaults[PICO8_PALETTE_COLORS] = {
    {0x00, 0x00, 0x00}, {0x1D, 0x2B, 0x53}, {0x7E, 0x25, 0x53}, {0x00, 0x87, 0x51},
    {0xAB, 0x52, 0x36}, {0x5F, 0x57, 0x4F}, {0xC2, 0xC3, 0xC7}, {0xFF, 0xF1, 0xE8},
    {0xFF, 0x00, 0x4D}, {0xFF, 0xA3, 0x00}, {0xFF, 0xEC, 0x27}, {0x00, 0xE4, 0x36},
    {0x29, 0xAD, 0xFF}, {0x83, 0x76, 0x9C}, {0xFF, 0x77, 0xA8}, {0xFF, 0xCC, 0xAA},
};

static pico8_color_t s_pico8_palette[PICO8_PALETTE_COLORS];

// Forward declarations
static size_t get_next_asset_index(size_t current_index);
static void swap_buffers(void);
static esp_err_t load_animation_into_buffer(size_t asset_index, animation_buffer_t *buf);
static void unload_animation_buffer(animation_buffer_t *buf);
static esp_err_t prefetch_first_frame(animation_buffer_t *buf);
static int render_next_frame(animation_buffer_t *buf, uint8_t *dest_buffer, int target_w, int target_h, bool use_prefetched);
static void discard_failed_swap_request(size_t failed_asset_index, esp_err_t error);
static void wait_for_loader_idle(void);
static bool is_sd_export_locked(void);
static esp_err_t refresh_animation_file_list(void);
static esp_err_t ensure_pico8_resources(void);
static void release_pico8_resources(void);
static bool pico8_stream_should_render(void);
static int render_pico8_frame(uint8_t *dest_buffer);

#if defined(CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS)
static const uint8_t digit_font[10][DIGIT_HEIGHT] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x1F},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
typedef uint16_t app_lcd_color_t;

static inline app_lcd_color_t app_lcd_make_color(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb565(r, g, b);
}

static inline void app_lcd_store_pixel(uint8_t *frame, int x, int y, app_lcd_color_t color)
{
    uint16_t *dst_row = (uint16_t *)(frame + (size_t)y * s_frame_row_stride_bytes);
    const size_t row_pixels = s_frame_row_stride_bytes / sizeof(uint16_t);
    if ((size_t)x >= row_pixels) {
        return;
    }
    dst_row[x] = color;
}
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
typedef uint32_t app_lcd_color_t;

static inline app_lcd_color_t app_lcd_make_color(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void app_lcd_store_pixel(uint8_t *frame, int x, int y, app_lcd_color_t color)
{
    uint8_t *row = frame + (size_t)y * s_frame_row_stride_bytes;
    const size_t idx = (size_t)x * 3U;
    if ((idx + 2) >= s_frame_row_stride_bytes) {
        return;
    }
    row[idx + 0] = (uint8_t)color;
    row[idx + 1] = (uint8_t)(color >> 8);
    row[idx + 2] = (uint8_t)(color >> 16);
}
#else
#error "Unsupported LCD pixel format"
#endif

static inline void draw_pixel(uint8_t *frame, int x, int y, app_lcd_color_t color)
{
    if (!frame) {
        return;
    }
    if (x < 0 || x >= EXAMPLE_LCD_H_RES || y < 0 || y >= EXAMPLE_LCD_V_RES) {
        return;
    }
    app_lcd_store_pixel(frame, x, y, color);
}

static int char_pixel_width(char c, int scale)
{
    if (scale <= 0) {
        scale = 1;
    }
    if (c >= '0' && c <= '9') {
        return (DIGIT_WIDTH * scale) + scale;
    }
    if (c == '.' || c == ',') {
        return scale * 2;
    }
    if (c == '-') {
        return (DIGIT_WIDTH * scale) + scale;
    }
    return scale * 3;
}

static void draw_char(uint8_t *frame, char c, int x, int y, int scale, app_lcd_color_t color)
{
    if (!frame) {
        return;
    }
    if (scale <= 0) {
        scale = 1;
    }
    if (c == ' ') {
        return;
    }
    if (c == '.' || c == ',') {
        const int dot_size = MAX(1, scale / 2);
        const int base_x = x;
        const int base_y = y + (DIGIT_HEIGHT * scale) - dot_size - 1;
        for (int dy = 0; dy < dot_size; ++dy) {
            for (int dx = 0; dx < dot_size; ++dx) {
                draw_pixel(frame, base_x + dx, base_y + dy, color);
            }
        }
        return;
    }
    if (c == '-') {
        const int line_height = MAX(1, scale / 2);
        const int base_y = y + (DIGIT_HEIGHT * scale) / 2;
        for (int dy = 0; dy < line_height; ++dy) {
            for (int dx = 0; dx < DIGIT_WIDTH * scale; ++dx) {
                draw_pixel(frame, x + dx, base_y + dy, color);
            }
        }
        return;
    }
    if (c < '0' || c > '9') {
        return;
    }

    const uint8_t *glyph = digit_font[c - '0'];
    for (int row = 0; row < DIGIT_HEIGHT; ++row) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < DIGIT_WIDTH; ++col) {
            if ((bits >> (DIGIT_WIDTH - 1 - col)) & 0x01) {
                const int px = x + col * scale;
                const int py = y + row * scale;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        draw_pixel(frame, px + dx, py + dy, color);
                    }
                }
            }
        }
    }
}

static void draw_text(uint8_t *frame, const char *text, int x, int y, int scale, app_lcd_color_t color)
{
    if (!frame || !text) {
        return;
    }
    if (scale <= 0) {
        scale = 1;
    }
    int cursor_x = x;
    for (const char *ch = text; *ch; ++ch) {
        draw_char(frame, *ch, cursor_x, y, scale, color);
        cursor_x += char_pixel_width(*ch, scale);
    }
}

static int measure_text_width(const char *text, int scale)
{
    if (!text || scale <= 0) {
        return 0;
    }
    int width = 0;
    for (const char *ch = text; *ch; ++ch) {
        width += char_pixel_width(*ch, scale);
    }
    return width;
}

static void draw_text_top_right(uint8_t *frame, const char *text, int margin_x, int margin_y, int scale, app_lcd_color_t color)
{
    if (!frame || !text) {
        return;
    }
    
    if (scale <= 0) {
        scale = 1;
    }
    const int width = measure_text_width(text, scale);
    int draw_x = EXAMPLE_LCD_H_RES - margin_x - width;
    if (draw_x < 0) {
        draw_x = 0;
    }
    draw_text(frame, text, draw_x, margin_y, scale, color);
}
#endif // CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS

static void blit_webp_frame_rows(const uint8_t *src_rgba, int src_w, int src_h,
                                 uint8_t *dst_buffer, int dst_w, int dst_h,
                                 int row_start, int row_end,
                                 const uint16_t *lookup_x, const uint16_t *lookup_y)
{
    if (!src_rgba || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    
    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;
    
    if (!lookup_x || !lookup_y) {
        ESP_LOGE(TAG, "Upscale lookup tables not initialized");
        return;
    }
    
    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
        const uint16_t src_y = lookup_y[dst_y];
        const uint8_t *src_row = src_rgba + (size_t)src_y * src_w * 4;
        
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)dst_y * s_frame_row_stride_bytes);
        for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
            const uint16_t src_x = lookup_x[dst_x];
            const uint8_t *pixel = src_row + (size_t)src_x * 4;
            dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
        }
#else
        uint8_t *dst_row = dst_buffer + (size_t)dst_y * s_frame_row_stride_bytes;
        for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
            const uint16_t src_x = lookup_x[dst_x];
            const uint8_t *pixel = src_row + (size_t)src_x * 4;
            const size_t idx = (size_t)dst_x * 3U;
            if ((idx + 2) < s_frame_row_stride_bytes) {
                dst_row[idx + 0] = pixel[2]; // B
                dst_row[idx + 1] = pixel[1]; // G
                dst_row[idx + 2] = pixel[0]; // R
            }
        }
#endif
    }
}

static void upscale_worker_top_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 0);
    
    while (true) {
        // Wait for notification using bit-based API to match sender
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);
        
        // Memory barrier to ensure we see all shared variables set by main task
        MEMORY_BARRIER();
        
        if (s_upscale_src_buffer && s_upscale_dst_buffer && 
            s_upscale_row_start_top < s_upscale_row_end_top) {
            blit_webp_frame_rows(s_upscale_src_buffer,
                                s_upscale_src_w, s_upscale_src_h,
                                s_upscale_dst_buffer,
                                EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                s_upscale_row_start_top, s_upscale_row_end_top,
                                s_upscale_lookup_x, s_upscale_lookup_y);
        }
        
        // Memory barrier to ensure all writes to dst_buffer are visible to other cores/DMA
        MEMORY_BARRIER();
        
        s_upscale_worker_top_done = true;
        if (s_upscale_main_task) {
            xTaskNotify(s_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

static void upscale_worker_bottom_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 1);
    
    while (true) {
        // Wait for notification using bit-based API to match sender
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);
        
        // Memory barrier to ensure we see all shared variables set by main task
        MEMORY_BARRIER();
        
        if (s_upscale_src_buffer && s_upscale_dst_buffer && 
            s_upscale_row_start_bottom < s_upscale_row_end_bottom) {
            blit_webp_frame_rows(s_upscale_src_buffer,
                                s_upscale_src_w, s_upscale_src_h,
                                s_upscale_dst_buffer,
                                EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                s_upscale_row_start_bottom, s_upscale_row_end_bottom,
                                s_upscale_lookup_x, s_upscale_lookup_y);
        }
        
        // Memory barrier to ensure all writes to dst_buffer are visible to other cores/DMA
        MEMORY_BARRIER();
        
        s_upscale_worker_bottom_done = true;
        if (s_upscale_main_task) {
            xTaskNotify(s_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

// Render next frame from animation buffer
static int render_next_frame(animation_buffer_t *buf, uint8_t *dest_buffer, int target_w, int target_h, bool use_prefetched)
{
    if (!buf || !buf->ready || !dest_buffer || !buf->decoder) {
        return -1;
    }
    
    // If prefetched frame is available and we're on the first frame, use it
    if (use_prefetched && buf->first_frame_ready && buf->prefetched_first_frame) {
        memcpy(dest_buffer, buf->prefetched_first_frame, s_frame_buffer_bytes);
        buf->first_frame_ready = false;  // Clear flag so we don't use it again
        return (int)buf->prefetched_first_frame_delay_ms;
    }
    
    if (!buf->native_frame_b1 || !buf->native_frame_b2) {
        ESP_LOGE(TAG, "Native frame buffers not allocated");
        return -1;
    }

    uint8_t *decode_buffer = (buf->native_buffer_active == 0) ? buf->native_frame_b1 : buf->native_frame_b2;
    
    esp_err_t err = animation_decoder_decode_next(buf->decoder, decode_buffer);
    if (err == ESP_ERR_INVALID_STATE) {
        // End of animation, reset
        animation_decoder_reset(buf->decoder);
        err = animation_decoder_decode_next(buf->decoder, decode_buffer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Animation decoder could not restart");
            return -1;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode frame: %s", esp_err_to_name(err));
        return -1;
    }
    
    // Get frame delay after decoding
    uint32_t frame_delay_ms = 1;
    esp_err_t delay_err = animation_decoder_get_frame_delay(buf->decoder, &frame_delay_ms);
    if (delay_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get frame delay, using default");
        frame_delay_ms = 1;
    }
    buf->current_frame_delay_ms = frame_delay_ms;
    
    buf->native_buffer_active = (buf->native_buffer_active == 0) ? 1 : 0;
    
    const uint8_t *src_for_upscale = decode_buffer;
    
    // Set up shared parameters for workers
    s_upscale_src_buffer = src_for_upscale;
    s_upscale_dst_buffer = dest_buffer;
    s_upscale_lookup_x = buf->upscale_lookup_x;
    s_upscale_lookup_y = buf->upscale_lookup_y;
    s_upscale_src_w = buf->upscale_src_w;
    s_upscale_src_h = buf->upscale_src_h;
    s_upscale_main_task = xTaskGetCurrentTaskHandle();
    
    const int dst_h = target_h;
    const int mid_row = dst_h / 2;
    
    s_upscale_worker_top_done = false;
    s_upscale_worker_bottom_done = false;
    
    s_upscale_row_start_top = 0;
    s_upscale_row_end_top = mid_row;
    s_upscale_row_start_bottom = mid_row;
    s_upscale_row_end_bottom = dst_h;
    
    // Memory barrier to ensure all shared variables are visible to worker cores
    MEMORY_BARRIER();
    
    // Notify BOTH workers simultaneously (back-to-back) to minimize timing skew
    // Critical: we want both workers to start as close together as possible
    // to reduce the chance of DMA catching the buffer in a partially-updated state
    if (s_upscale_worker_top && s_upscale_worker_bottom) {
        xTaskNotify(s_upscale_worker_top, 1, eSetBits);
        xTaskNotify(s_upscale_worker_bottom, 1, eSetBits);
    }
    
    // Wait for both workers to complete using proper notification API
    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;
    
    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            // Timeout - yield to allow idle task to reset watchdog
            taskYIELD();
        }
    }
    
    if (!s_upscale_worker_top_done || !s_upscale_worker_bottom_done) {
        ESP_LOGW(TAG, "Upscale workers may not have completed properly");
    }
    
    // Memory barrier to ensure all worker writes are visible before DMA
    MEMORY_BARRIER();

    return (int)buf->current_frame_delay_ms;
}

static bool pico8_stream_should_render(void)
{
    if (!s_pico8_override_active || !s_pico8_frame_ready) {
        return false;
    }

    bool active = false;
    int64_t now = esp_timer_get_time();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_pico8_override_active && s_pico8_frame_ready &&
            (now - s_pico8_last_frame_time_us) <= PICO8_STREAM_TIMEOUT_US) {
            active = true;
        } else if (s_pico8_override_active &&
                   (now - s_pico8_last_frame_time_us) > PICO8_STREAM_TIMEOUT_US) {
            s_pico8_override_active = false;
            s_pico8_frame_ready = false;
        }
        xSemaphoreGive(s_buffer_mutex);
    }

    return active;
}

static int render_pico8_frame(uint8_t *dest_buffer)
{
    if (!dest_buffer) {
        return -1;
    }

    if (ensure_pico8_resources() != ESP_OK) {
        return -1;
    }

    uint8_t *src = NULL;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        src = s_pico8_frame_buffers[s_pico8_display_index];
        xSemaphoreGive(s_buffer_mutex);
    } else {
        src = s_pico8_frame_buffers[s_pico8_display_index];
    }

    if (!src) {
        return -1;
    }

    const int dst_h = EXAMPLE_LCD_V_RES;
    const int mid_row = dst_h / 2;

    s_upscale_src_buffer = src;
    s_upscale_dst_buffer = dest_buffer;
    s_upscale_lookup_x = s_pico8_lookup_x;
    s_upscale_lookup_y = s_pico8_lookup_y;
    s_upscale_src_w = PICO8_FRAME_WIDTH;
    s_upscale_src_h = PICO8_FRAME_HEIGHT;
    s_upscale_main_task = xTaskGetCurrentTaskHandle();

    s_upscale_worker_top_done = false;
    s_upscale_worker_bottom_done = false;

    s_upscale_row_start_top = 0;
    s_upscale_row_end_top = mid_row;
    s_upscale_row_start_bottom = mid_row;
    s_upscale_row_end_bottom = dst_h;

    MEMORY_BARRIER();

    if (s_upscale_worker_top) {
        xTaskNotify(s_upscale_worker_top, 1, eSetBits);
    }
    if (s_upscale_worker_bottom) {
        xTaskNotify(s_upscale_worker_bottom, 1, eSetBits);
    }

    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;

    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            taskYIELD();
        }
    }

    MEMORY_BARRIER();
    return 16; // target ~60 FPS
}

static bool lcd_panel_refresh_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t higher_prio_task_woken = pdFALSE;
    if (sem) {
        xSemaphoreGiveFromISR(sem, &higher_prio_task_woken);
    }
    return higher_prio_task_woken == pdTRUE;
}

// Forward declarations - must be before functions that use them
// Discard a failed swap request and restore system to responsive state
static void discard_failed_swap_request(size_t failed_asset_index, esp_err_t error)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // Clear swap request flag - this swap attempt failed
        bool had_swap_request = s_swap_requested;
        s_swap_requested = false;
        
        // Reset loader busy flag - loader is done with this attempt
        s_loader_busy = false;
        
        // Ensure back buffer is in a clean state (unload any partial state)
        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }
        
        // Advance to next asset index for future attempts
        size_t next_index = get_next_asset_index(failed_asset_index);
        s_next_asset_index = next_index;
        
        // Check if no healthy files are available
        if (next_index == failed_asset_index && s_sd_file_list.health_flags) {
            // Check if there are any healthy files at all
            bool any_healthy = false;
            for (size_t i = 0; i < s_sd_file_list.count; i++) {
                if (s_sd_file_list.health_flags[i]) {
                    any_healthy = true;
                    break;
                }
            }
            if (!any_healthy) {
                ESP_LOGW(TAG, "No healthy animation files available. System remains responsive but will not auto-swap.");
            }
        }
        
        xSemaphoreGive(s_buffer_mutex);
        
        // Log the discard event
        if (had_swap_request) {
            ESP_LOGW(TAG, "Discarded swap request for animation index %zu (error: %s). System remains responsive.", 
                     failed_asset_index, esp_err_to_name(error));
        } else {
            ESP_LOGW(TAG, "Failed to load animation index %zu (error: %s). System remains responsive.", 
                     failed_asset_index, esp_err_to_name(error));
        }
    }
}

static void wait_for_loader_idle(void)
{
    while (true) {
        bool busy = false;
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            busy = s_loader_busy;
            xSemaphoreGive(s_buffer_mutex);
        }
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool is_sd_export_locked(void)
{
    bool locked = s_sd_export_active;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        locked = s_sd_export_active;
        xSemaphoreGive(s_buffer_mutex);
    }
    return locked;
}

// Background loader task - loads animations into back buffer and prefetches first frame
static void animation_loader_task(void *arg)
{
    (void)arg;
    
    while (true) {
        // Wait for signal to load next animation
        if (xSemaphoreTake(s_loader_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        
        size_t asset_index_to_load;
        bool swap_was_requested = false;
        
        // Get the asset index to load
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            asset_index_to_load = s_next_asset_index;
            swap_was_requested = s_swap_requested;
            s_loader_busy = true;  // Mark loader as busy
            xSemaphoreGive(s_buffer_mutex);
        } else {
            continue;
        }
        
        ESP_LOGD(TAG, "Loader task: Loading animation index %zu into back buffer", asset_index_to_load);
        
        // Load animation into back buffer
        esp_err_t err = load_animation_into_buffer(asset_index_to_load, &s_back_buffer);
        if (err != ESP_OK) {
            // Discard the failed swap request and restore system to responsive state
            discard_failed_swap_request(asset_index_to_load, err);
            continue;
        }
        
        // Mark buffer as needing prefetch (will be done by render task to avoid race condition)
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            s_back_buffer.prefetch_pending = true;
            s_back_buffer.ready = false;  // Not ready until prefetch completes
            // If swap was requested, keep the flag set so render loop performs swap after prefetch
            if (swap_was_requested) {
                s_swap_requested = true;
                ESP_LOGD(TAG, "Loader task: Swap was requested, will swap after prefetch");
            }
            s_loader_busy = false;  // Mark loader as not busy (prefetch will be handled by render task)
            xSemaphoreGive(s_buffer_mutex);
        }
        
        ESP_LOGD(TAG, "Loader task: Successfully loaded animation index %zu (prefetch_pending=true)", asset_index_to_load);
    }
}

static void lcd_animation_task(void *arg)
{
    (void)arg;
#if defined(CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS)
    const app_lcd_color_t color_red = app_lcd_make_color(0xFF, 0x20, 0x20);
    const app_lcd_color_t color_white = app_lcd_make_color(0xFF, 0xFF, 0xFF);
#endif

    const bool use_vsync = (s_buffer_count > 1) && (s_vsync_sem != NULL);
    const uint8_t buffer_count = (s_buffer_count == 0) ? 1 : s_buffer_count;
    bool use_prefetched = false;  // Track if we should use prefetched frame after swap

    while (true) {
        if (use_vsync) {
            xSemaphoreTake(s_vsync_sem, portMAX_DELAY);
        }

        bool paused_local = false;
        bool swap_requested = false;
        bool back_buffer_ready = false;
        bool back_buffer_prefetch_pending = false;
        
        // Check for swap request and buffer state (must hold mutex for atomic check)
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            paused_local = s_anim_paused;
            swap_requested = s_swap_requested;
            back_buffer_ready = s_back_buffer.ready;
            back_buffer_prefetch_pending = s_back_buffer.prefetch_pending;
            xSemaphoreGive(s_buffer_mutex);
        }

        // Handle prefetch if pending (must be done in render task to avoid race with upscale workers)
        if (back_buffer_prefetch_pending) {
            esp_err_t prefetch_err = prefetch_first_frame(&s_back_buffer);
            if (prefetch_err == ESP_OK) {
                // Prefetch successful, mark buffer as ready
                if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                    s_back_buffer.prefetch_pending = false;
                    s_back_buffer.ready = true;
                    // If swap was requested, it's now ready to swap
                    if (s_swap_requested) {
                        ESP_LOGD(TAG, "Render task: Prefetch complete, swap ready");
                    }
                    xSemaphoreGive(s_buffer_mutex);
                }
            } else {
                ESP_LOGW(TAG, "Render task: Prefetch failed: %s", esp_err_to_name(prefetch_err));
                // Mark prefetch as done even on failure, so we don't retry forever
                if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                    s_back_buffer.prefetch_pending = false;
                    s_back_buffer.ready = true;  // Allow swap even if prefetch failed
                    xSemaphoreGive(s_buffer_mutex);
                }
            }
            // Re-read state after prefetch
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                swap_requested = s_swap_requested;
                back_buffer_ready = s_back_buffer.ready;
                xSemaphoreGive(s_buffer_mutex);
                ESP_LOGD(TAG, "Render task: Prefetch completed, buffer ready");
            }
        }

        // Perform buffer swap if requested and back buffer is ready
        if (swap_requested && back_buffer_ready) {
            swap_buffers();
            use_prefetched = true;  // Use prefetched frame on first render after swap
            // Note: Next animation will be loaded on-demand when next swap gesture occurs
        }

        uint8_t *frame = NULL;
        int frame_delay_ms = 1;
        uint32_t prev_frame_delay_ms = s_target_frame_delay_ms;  // Track delay of frame currently on screen

        bool pico8_active = pico8_stream_should_render();

        // If paused but a swap just happened, render the prefetched first frame once
        if (pico8_active) {
            frame = s_lcd_buffers[s_render_buffer_index];
            if (frame) {
                frame_delay_ms = render_pico8_frame(frame);
                if (frame_delay_ms < 0) {
                    frame_delay_ms = 16;
                }
                s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
                s_last_display_buffer = s_render_buffer_index;
                s_render_buffer_index = (s_render_buffer_index + 1) % buffer_count;
#if APP_LCD_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
                esp_err_t msync_err = esp_cache_msync(frame, s_frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                if (msync_err != ESP_OK) {
                    ESP_LOGW(TAG, "Cache sync failed (pico8): %s", esp_err_to_name(msync_err));
                }
#endif
            }
        } else if (paused_local && use_prefetched && s_front_buffer.ready) {
            frame = s_lcd_buffers[s_render_buffer_index];
            if (frame) {
                frame_delay_ms = render_next_frame(&s_front_buffer, frame, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, true);
                use_prefetched = false;  // Only use prefetched frame once
                if (frame_delay_ms < 0) {
                    frame_delay_ms = 100;  // Default delay when paused
                }
                s_target_frame_delay_ms = 100;  // Use 100ms delay when paused
                s_last_display_buffer = s_render_buffer_index;
                s_render_buffer_index = (s_render_buffer_index + 1) % buffer_count;

#if APP_LCD_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
                esp_err_t msync_err = esp_cache_msync(frame, s_frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                if (msync_err != ESP_OK) {
                    ESP_LOGW(TAG, "Cache sync failed: %s", esp_err_to_name(msync_err));
                }
#endif
            }
        } else if (!paused_local && s_front_buffer.ready) {
            // Record when frame processing starts
            s_frame_processing_start_us = esp_timer_get_time();
            
            frame = s_lcd_buffers[s_render_buffer_index];
            if (frame) {
                // Save previous frame delay before decoding next frame
                prev_frame_delay_ms = s_target_frame_delay_ms;
                
                frame_delay_ms = render_next_frame(&s_front_buffer, frame, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, use_prefetched);
                use_prefetched = false;  // Only use prefetched frame once
                if (frame_delay_ms < 0) {
                    frame_delay_ms = 1;
                }
                s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
                s_latest_frame_duration_ms = frame_delay_ms;
#if defined(CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS)
                const int text_scale = 3;
                const int margin = text_scale * 2;

                app_lcd_color_t color_text = color_white;
                if (swap_requested) {
                    color_text = color_red;
                }

                draw_text_top_right(frame, s_frame_duration_text, margin, margin, text_scale, color_text);
#endif

#if APP_LCD_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
                esp_err_t msync_err = esp_cache_msync(frame, s_frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                if (msync_err != ESP_OK) {
                    ESP_LOGW(TAG, "Cache sync failed: %s", esp_err_to_name(msync_err));
                }
#endif
                s_last_display_buffer = s_render_buffer_index;
                s_render_buffer_index = (s_render_buffer_index + 1) % buffer_count;
            }
        } else {
            // Paused and no swap, or buffer not ready - reuse last frame
            uint8_t reuse_index = s_last_display_buffer;
            if (reuse_index >= buffer_count) {
                reuse_index = 0;
            }
            frame = s_lcd_buffers[reuse_index];
            frame_delay_ms = 100;  // Use 100ms delay when paused
            s_target_frame_delay_ms = 100;
            s_last_frame_present_us = 0;
            s_frame_processing_start_us = 0;
        }

        if (!frame) {
            s_last_frame_present_us = 0;
            s_frame_processing_start_us = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Calculate residual wait time before DMA
        // Use previous frame's delay since that's the frame currently on screen
        if (!paused_local && s_front_buffer.ready && !APP_LCD_MAX_SPEED_PLAYBACK_ENABLED) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t processing_time_us = now_us - s_frame_processing_start_us;
            const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;
            
            if (processing_time_us < target_delay_us) {
                // Need to wait for the remainder
                const int64_t residual_us = target_delay_us - processing_time_us;
                if (residual_us > 0) {
                    // Convert to milliseconds, with minimum 1 tick
                    const TickType_t residual_ticks = pdMS_TO_TICKS((residual_us + 500) / 1000);
                    if (residual_ticks > 0) {
                        vTaskDelay(residual_ticks);
                    }
                }
            }
            // If processing_time_us >= target_delay_us, we've already exceeded target, skip wait
        }
        const bool blank_display = (app_lcd_get_brightness() == 0);
        if (blank_display) {
            memset(frame, 0, s_frame_buffer_bytes);
#if APP_LCD_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
            esp_err_t blank_msync_err = esp_cache_msync(frame, s_frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            if (blank_msync_err != ESP_OK) {
                ESP_LOGW(TAG, "Cache sync failed (blank frame): %s", esp_err_to_name(blank_msync_err));
            }
#endif
        }

        esp_err_t draw_err = esp_lcd_panel_draw_bitmap(s_display_handle, 0, 0,
                                                       EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, frame);
        
        if (draw_err != ESP_OK) {
            ESP_LOGE(TAG, "Panel draw failed: %s", esp_err_to_name(draw_err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Record DMA completion time and calculate frame duration
        if (!paused_local && s_front_buffer.ready) {
            const int64_t now_us = esp_timer_get_time();

            // Update duration display (use actual measured time between DMA completions)
            if (s_last_frame_present_us != 0) {
                const int64_t frame_delta_us = now_us - s_last_frame_present_us;
                s_latest_frame_duration_ms = (int)((frame_delta_us + 500) / 1000);
            }
            s_last_frame_present_us = now_us;

            if (s_last_duration_update_us == 0) {
                s_last_duration_update_us = now_us;
            }
            if ((now_us - s_last_duration_update_us) >= 500000) {
                snprintf(s_frame_duration_text, sizeof(s_frame_duration_text), "%d", s_latest_frame_duration_ms);
                s_last_duration_update_us = now_us;
            }
        }

        TickType_t delay_ticks;
        if (paused_local) {
            // When paused, present the still frame every 100ms without blocking other tasks
            delay_ticks = pdMS_TO_TICKS(100);
        } else if (APP_LCD_MAX_SPEED_PLAYBACK_ENABLED) {
            delay_ticks = 1;
        } else {
            // No additional delay needed - residual wait already handled before DMA
            delay_ticks = 1;
        }
        if (delay_ticks == 0) {
            delay_ticks = 1;
        }

        vTaskDelay(delay_ticks);
    }
}

static void free_sd_file_list(void)
{
    if (s_sd_file_list.filenames) {
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            free(s_sd_file_list.filenames[i]);
        }
        free(s_sd_file_list.filenames);
        s_sd_file_list.filenames = NULL;
    }
    if (s_sd_file_list.types) {
        free(s_sd_file_list.types);
        s_sd_file_list.types = NULL;
    }
    if (s_sd_file_list.health_flags) {
        free(s_sd_file_list.health_flags);
        s_sd_file_list.health_flags = NULL;
    }
    s_sd_file_list.count = 0;
    s_sd_file_list.current_index = 0;
    if (s_sd_file_list.animations_dir) {
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.animations_dir = NULL;
    }
}

static asset_type_t get_asset_type(const char *filename)
{
    size_t len = strlen(filename);
    if (len >= 5 && strcasecmp(filename + len - 5, ".webp") == 0) {
        return ASSET_TYPE_WEBP;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".gif") == 0) {
        return ASSET_TYPE_GIF;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".png") == 0) {
        return ASSET_TYPE_PNG;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".jpg") == 0) {
        return ASSET_TYPE_JPEG;
    }
    if (len >= 5 && strcasecmp(filename + len - 5, ".jpeg") == 0) {
        return ASSET_TYPE_JPEG;
    }
    return ASSET_TYPE_WEBP; // Default
}

static bool directory_has_animation_files(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "directory_has_animation_files: Failed to open %s", dir_path);
        return false;
    }

    struct dirent *entry;
    bool has_anim = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            const char *name = entry->d_name;
            size_t len = strlen(name);
            if ((len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0)) {
                has_anim = true;
                break;
            }
        }
    }
    closedir(dir);
    
    return has_anim;
}

static esp_err_t find_animations_directory(const char *root_path, char **found_dir_out)
{
    ESP_LOGI(TAG, "Searching in: %s", root_path);
    
    DIR *dir = opendir(root_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", root_path, errno);
        return ESP_FAIL;
    }

    if (directory_has_animation_files(root_path)) {
        size_t len = strlen(root_path);
        *found_dir_out = (char *)malloc(len + 1);
        if (!*found_dir_out) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        strcpy(*found_dir_out, root_path);
        closedir(dir);
        ESP_LOGI(TAG, "Found animations directory: %s", *found_dir_out);
        return ESP_OK;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char subdir_path[512];
        int ret = snprintf(subdir_path, sizeof(subdir_path), "%s/%s", root_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(subdir_path)) {
            continue;
        }

        struct stat st;
        if (stat(subdir_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = find_animations_directory(subdir_path, found_dir_out);
            if (err == ESP_OK) {
                closedir(dir);
                return ESP_OK;
            }
        }
    }
    
    closedir(dir);
    return ESP_ERR_NOT_FOUND;
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

// Shuffle the animation file list using Fisher-Yates algorithm
static void shuffle_animation_file_list(void)
{
    if (s_sd_file_list.count <= 1) {
        return;  // Nothing to shuffle
    }
    
    // Fisher-Yates shuffle
    for (size_t i = s_sd_file_list.count - 1; i > 0; i--) {
        size_t j = esp_random() % (i + 1);
        
        // Swap filenames
        char *temp_filename = s_sd_file_list.filenames[i];
        s_sd_file_list.filenames[i] = s_sd_file_list.filenames[j];
        s_sd_file_list.filenames[j] = temp_filename;
        
        // Swap types to keep them in sync
        asset_type_t temp_type = s_sd_file_list.types[i];
        s_sd_file_list.types[i] = s_sd_file_list.types[j];
        s_sd_file_list.types[j] = temp_type;
        
        // Swap health flags to keep them in sync
        if (s_sd_file_list.health_flags) {
            bool temp_health = s_sd_file_list.health_flags[i];
            s_sd_file_list.health_flags[i] = s_sd_file_list.health_flags[j];
            s_sd_file_list.health_flags[j] = temp_health;
        }
    }
    
    ESP_LOGI(TAG, "Randomized animation file list order");
}


static esp_err_t enumerate_animation_files(const char *dir_path)
{
    free_sd_file_list();

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_FAIL;
    }

    struct dirent *entry;
    size_t anim_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            const char *name = entry->d_name;
            size_t len = strlen(name);
            if ((len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0)) {
                anim_count++;
            }
        }
    }
    rewinddir(dir);

    if (anim_count == 0) {
        ESP_LOGW(TAG, "No animation files found in %s", dir_path);
        closedir(dir);
        return ESP_ERR_NOT_FOUND;
    }

    size_t dir_path_len = strlen(dir_path);
    s_sd_file_list.animations_dir = (char *)malloc(dir_path_len + 1);
    if (!s_sd_file_list.animations_dir) {
        ESP_LOGE(TAG, "Failed to allocate directory path string");
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    strcpy(s_sd_file_list.animations_dir, dir_path);

    s_sd_file_list.filenames = (char **)malloc(anim_count * sizeof(char *));
    if (!s_sd_file_list.filenames) {
        ESP_LOGE(TAG, "Failed to allocate filename array");
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.animations_dir = NULL;
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    s_sd_file_list.types = (asset_type_t *)malloc(anim_count * sizeof(asset_type_t));
    if (!s_sd_file_list.types) {
        ESP_LOGE(TAG, "Failed to allocate type array");
        free(s_sd_file_list.filenames);
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.filenames = NULL;
        s_sd_file_list.animations_dir = NULL;
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    s_sd_file_list.health_flags = (bool *)malloc(anim_count * sizeof(bool));
    if (!s_sd_file_list.health_flags) {
        ESP_LOGE(TAG, "Failed to allocate health flags array");
        free(s_sd_file_list.filenames);
        free(s_sd_file_list.types);
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.filenames = NULL;
        s_sd_file_list.types = NULL;
        s_sd_file_list.animations_dir = NULL;
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize all files as healthy (file is ok)
    for (size_t i = 0; i < anim_count; i++) {
        s_sd_file_list.health_flags[i] = true;
    }

    size_t idx = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            const char *name = entry->d_name;
            size_t len = strlen(name);
            bool is_anim = false;
            if (len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) {
                is_anim = true;
            } else if (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) {
                is_anim = true;
            } else if (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) {
                is_anim = true;
            } else if (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) {
                is_anim = true;
            } else if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) {
                is_anim = true;
            }
            
            if (is_anim) {
                size_t name_len = strlen(name);
                s_sd_file_list.filenames[idx] = (char *)malloc(name_len + 1);
                if (!s_sd_file_list.filenames[idx]) {
                    for (size_t i = 0; i < idx; i++) {
                        free(s_sd_file_list.filenames[i]);
                    }
                    free(s_sd_file_list.filenames);
                    free(s_sd_file_list.types);
                    free(s_sd_file_list.health_flags);
                    free(s_sd_file_list.animations_dir);
                    s_sd_file_list.filenames = NULL;
                    s_sd_file_list.types = NULL;
                    s_sd_file_list.health_flags = NULL;
                    s_sd_file_list.animations_dir = NULL;
                    closedir(dir);
                    return ESP_ERR_NO_MEM;
                }
                strcpy(s_sd_file_list.filenames[idx], name);
                s_sd_file_list.types[idx] = get_asset_type(name);
                idx++;
            }
        }
    }
    closedir(dir);

    s_sd_file_list.count = anim_count;

    qsort(s_sd_file_list.filenames, s_sd_file_list.count, sizeof(char *), compare_strings);
    // Re-sort types array to match sorted filenames
    // Note: health_flags don't need re-sorting since they're all initialized to true
    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        s_sd_file_list.types[i] = get_asset_type(s_sd_file_list.filenames[i]);
    }

    ESP_LOGI(TAG, "Found %zu animation files in %s", s_sd_file_list.count, dir_path);
    // for (size_t i = 0; i < s_sd_file_list.count; i++) {
    //     ESP_LOGI(TAG, "  [%zu] %s (%s)", i, s_sd_file_list.filenames[i],
    //              s_sd_file_list.types[i] == ASSET_TYPE_WEBP ? "WebP" : "GIF");
    // }

    // Randomize the file list order after enumeration
    shuffle_animation_file_list();

    s_sd_file_list.current_index = 0;
    esp_err_t pico_err = ensure_pico8_resources();
    if (pico_err != ESP_OK) {
        ESP_LOGW(TAG, "PICO-8 buffer init failed: %s", esp_err_to_name(pico_err));
    }

    return ESP_OK;
}

// ============================================================================
// TEMPORARY DEBUG FUNCTION - REMOVE AFTER TESTING
// ============================================================================
// This function filters the file list to only keep specific files for testing:
// - smb2-jump-cat-64p-L32.jpg
// - 1 gif file
// - 1 png file
// - 1 webp file
// - 5 other random files
// ============================================================================
#if 0
static void filter_file_list_for_debug(void)
{
    if (s_sd_file_list.count == 0 || !s_sd_file_list.filenames || 
        !s_sd_file_list.types || !s_sd_file_list.health_flags) {
        return;
    }

    // Arrays to store indices of files to keep
    size_t keep_indices[9];  // 1 jpg + 1 gif + 1 png + 1 webp + 5 random = 9 max
    size_t keep_count = 0;
    bool found_jpg = false;
    bool found_gif = false;
    bool found_png = false;
    bool found_webp = false;
    size_t random_count = 0;

    // First pass: find specific files
    for (size_t i = 0; i < s_sd_file_list.count && keep_count < 9; i++) {
        const char *filename = s_sd_file_list.filenames[i];
        asset_type_t type = s_sd_file_list.types[i];

        // Look for the specific jpg file
        if (!found_jpg && strcmp(filename, "smb2-jump-cat-64p-L32.jpg") == 0) {
            keep_indices[keep_count++] = i;
            found_jpg = true;
            continue;
        }

        // Look for one of each type
        if (!found_gif && type == ASSET_TYPE_GIF) {
            keep_indices[keep_count++] = i;
            found_gif = true;
            continue;
        }

        if (!found_png && type == ASSET_TYPE_PNG) {
            keep_indices[keep_count++] = i;
            found_png = true;
            continue;
        }

        if (!found_webp && type == ASSET_TYPE_WEBP) {
            keep_indices[keep_count++] = i;
            found_webp = true;
            continue;
        }
    }

    // Second pass: add 5 random files (excluding already selected ones)
    // Build list of available indices
    size_t available_indices[s_sd_file_list.count];
    size_t available_count = 0;
    
    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        // Check if this file is already selected
        bool already_selected = false;
        for (size_t j = 0; j < keep_count; j++) {
            if (keep_indices[j] == i) {
                already_selected = true;
                break;
            }
        }
        if (!already_selected) {
            available_indices[available_count++] = i;
        }
    }
    
    // Randomly select 5 files from available ones
    while (random_count < 5 && keep_count < 9 && available_count > 0) {
        size_t random_idx = esp_random() % available_count;
        size_t selected_idx = available_indices[random_idx];
        
        keep_indices[keep_count++] = selected_idx;
        random_count++;
        
        // Remove selected index from available list by swapping with last and decrementing
        available_indices[random_idx] = available_indices[available_count - 1];
        available_count--;
    }

    if (keep_count == 0) {
        ESP_LOGW(TAG, "DEBUG: No files matched filter criteria, keeping original list");
        return;
    }

    // Create new arrays with only the selected files
    char **new_filenames = (char **)malloc(keep_count * sizeof(char *));
    asset_type_t *new_types = (asset_type_t *)malloc(keep_count * sizeof(asset_type_t));
    bool *new_health_flags = (bool *)malloc(keep_count * sizeof(bool));

    if (!new_filenames || !new_types || !new_health_flags) {
        ESP_LOGE(TAG, "DEBUG: Failed to allocate filtered file list arrays");
        free(new_filenames);
        free(new_types);
        free(new_health_flags);
        return;
    }

    // Copy selected files to new arrays
    for (size_t i = 0; i < keep_count; i++) {
        size_t src_idx = keep_indices[i];
        size_t name_len = strlen(s_sd_file_list.filenames[src_idx]);
        new_filenames[i] = (char *)malloc(name_len + 1);
        if (!new_filenames[i]) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                free(new_filenames[j]);
            }
            free(new_filenames);
            free(new_types);
            free(new_health_flags);
            ESP_LOGE(TAG, "DEBUG: Failed to allocate filename in filtered list");
            return;
        }
        strcpy(new_filenames[i], s_sd_file_list.filenames[src_idx]);
        new_types[i] = s_sd_file_list.types[src_idx];
        new_health_flags[i] = s_sd_file_list.health_flags[src_idx];
    }

    // Free old arrays
    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        free(s_sd_file_list.filenames[i]);
    }
    free(s_sd_file_list.filenames);
    free(s_sd_file_list.types);
    free(s_sd_file_list.health_flags);

    // Replace with filtered arrays
    s_sd_file_list.filenames = new_filenames;
    s_sd_file_list.types = new_types;
    s_sd_file_list.health_flags = new_health_flags;
    s_sd_file_list.count = keep_count;
    s_sd_file_list.current_index = 0;

    ESP_LOGI(TAG, "DEBUG: Filtered file list to %zu files (1 jpg, 1 gif, 1 png, 1 webp, %zu random)", 
             keep_count, random_count);
#endif
// ============================================================================
// END TEMPORARY DEBUG FUNCTION
// ============================================================================

static esp_err_t load_animation_file_from_sd(const char *filepath, uint8_t **data_out, size_t *size_out)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = (uint8_t *)malloc((size_t)file_size);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %ld bytes for animation file", file_size);
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, f);
    fclose(f);

    if (bytes_read != (size_t)file_size) {
        ESP_LOGE(TAG, "Failed to read complete file: read %zu of %ld bytes", bytes_read, file_size);
        free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    *data_out = buffer;
    *size_out = (size_t)file_size;

    return ESP_OK;
}

static esp_err_t refresh_animation_file_list(void)
{
    if (!s_sd_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char *found_dir = NULL;
    esp_err_t err = find_animations_directory(BSP_SD_MOUNT_POINT, &found_dir);
    if (err != ESP_OK || !found_dir) {
        if (found_dir) {
            free(found_dir);
        }
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    esp_err_t enum_err = enumerate_animation_files(found_dir);
    free(found_dir);
    return enum_err;
}

// Helper function to unload a single animation buffer
static void unload_animation_buffer(animation_buffer_t *buf)
{
    if (!buf) {
        return;
    }
    
    animation_decoder_unload(&buf->decoder);
    
    if (buf->file_data) {
        free((void *)buf->file_data);
        buf->file_data = NULL;
        buf->file_size = 0;
    }
    
    free(buf->native_frame_b1);
    free(buf->native_frame_b2);
    buf->native_frame_b1 = NULL;
    buf->native_frame_b2 = NULL;
    buf->native_buffer_active = 0;
    buf->native_frame_size = 0;
    
    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);
    buf->upscale_lookup_x = NULL;
    buf->upscale_lookup_y = NULL;
    buf->upscale_src_w = 0;
    buf->upscale_src_h = 0;
    buf->upscale_dst_w = 0;
    buf->upscale_dst_h = 0;
    
    free(buf->prefetched_first_frame);
    buf->prefetched_first_frame = NULL;
    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;
    buf->prefetched_first_frame_delay_ms = 1;
    buf->current_frame_delay_ms = 1;
    
    buf->ready = false;
    memset(&buf->decoder_info, 0, sizeof(buf->decoder_info));
    buf->asset_index = 0;
}

static esp_err_t ensure_pico8_resources(void)
{
    const size_t frame_bytes = (size_t)PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT * 4;

    for (int i = 0; i < 2; ++i) {
        if (!s_pico8_frame_buffers[i]) {
            s_pico8_frame_buffers[i] = (uint8_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_pico8_frame_buffers[i]) {
                ESP_LOGE(TAG, "Failed to allocate PICO-8 frame buffer %d", i);
                return ESP_ERR_NO_MEM;
            }
            memset(s_pico8_frame_buffers[i], 0, frame_bytes);
        }
    }

    if (!s_pico8_lookup_x) {
        s_pico8_lookup_x = (uint16_t *)heap_caps_malloc(EXAMPLE_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!s_pico8_lookup_x) {
            ESP_LOGE(TAG, "Failed to allocate PICO-8 lookup X table");
            return ESP_ERR_NO_MEM;
        }
        for (int dst_x = 0; dst_x < EXAMPLE_LCD_H_RES; ++dst_x) {
            int src_x = (dst_x * PICO8_FRAME_WIDTH) / EXAMPLE_LCD_H_RES;
            if (src_x >= PICO8_FRAME_WIDTH) {
                src_x = PICO8_FRAME_WIDTH - 1;
            }
            s_pico8_lookup_x[dst_x] = (uint16_t)src_x;
        }
    }

    if (!s_pico8_lookup_y) {
        s_pico8_lookup_y = (uint16_t *)heap_caps_malloc(EXAMPLE_LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!s_pico8_lookup_y) {
            ESP_LOGE(TAG, "Failed to allocate PICO-8 lookup Y table");
            return ESP_ERR_NO_MEM;
        }
        for (int dst_y = 0; dst_y < EXAMPLE_LCD_V_RES; ++dst_y) {
            int src_y = (dst_y * PICO8_FRAME_HEIGHT) / EXAMPLE_LCD_V_RES;
            if (src_y >= PICO8_FRAME_HEIGHT) {
                src_y = PICO8_FRAME_HEIGHT - 1;
            }
            s_pico8_lookup_y[dst_y] = (uint16_t)src_y;
        }
    }

    if (!s_pico8_palette_initialized) {
        memcpy(s_pico8_palette, s_pico8_palette_defaults, sizeof(s_pico8_palette));
        s_pico8_palette_initialized = true;
    }

    return ESP_OK;
}

static void release_pico8_resources(void)
{
    for (int i = 0; i < 2; ++i) {
        if (s_pico8_frame_buffers[i]) {
            free(s_pico8_frame_buffers[i]);
            s_pico8_frame_buffers[i] = NULL;
        }
    }
    if (s_pico8_lookup_x) {
        heap_caps_free(s_pico8_lookup_x);
        s_pico8_lookup_x = NULL;
    }
    if (s_pico8_lookup_y) {
        heap_caps_free(s_pico8_lookup_y);
        s_pico8_lookup_y = NULL;
    }
    s_pico8_frame_ready = false;
    s_pico8_override_active = false;
    s_pico8_last_frame_time_us = 0;
    s_pico8_palette_initialized = false;
}

// Calculate next animation index in play order, skipping unhealthy files
// Returns current_index if no healthy files are found (to avoid infinite loops)
static size_t get_next_asset_index(size_t current_index)
{
    if (s_sd_file_list.count == 0) {
        return 0;
    }
    
    // If health flags aren't available, fall back to simple increment
    if (!s_sd_file_list.health_flags) {
        return (current_index + 1) % s_sd_file_list.count;
    }
    
    // Search for next healthy file, wrapping around if necessary
    size_t start_index = (current_index + 1) % s_sd_file_list.count;
    size_t checked = 0;
    
    while (checked < s_sd_file_list.count) {
        if (s_sd_file_list.health_flags[start_index]) {
            return start_index;  // Found a healthy file
        }
        start_index = (start_index + 1) % s_sd_file_list.count;
        checked++;
    }
    
    // No healthy files found, return current_index to avoid infinite loops
    return current_index;
}

// Calculate previous animation index in play order, skipping unhealthy files
// Returns current_index if no healthy files are found (to avoid infinite loops)
static size_t get_previous_asset_index(size_t current_index)
{
    if (s_sd_file_list.count == 0) {
        return 0;
    }
    
    // If health flags aren't available, fall back to simple decrement
    if (!s_sd_file_list.health_flags) {
        return (current_index == 0) ? (s_sd_file_list.count - 1) : (current_index - 1);
    }
    
    // Search for previous healthy file, wrapping around if necessary
    size_t start_index = (current_index == 0) ? (s_sd_file_list.count - 1) : (current_index - 1);
    size_t checked = 0;
    
    while (checked < s_sd_file_list.count) {
        if (s_sd_file_list.health_flags[start_index]) {
            return start_index;  // Found a healthy file
        }
        start_index = (start_index == 0) ? (s_sd_file_list.count - 1) : (start_index - 1);
        checked++;
    }
    
    // No healthy files found, return current_index to avoid infinite loops
    return current_index;
}

// Atomically swap front and back buffers
static void swap_buffers(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        animation_buffer_t temp = s_front_buffer;
        s_front_buffer = s_back_buffer;
        s_back_buffer = temp;
        
        // Clear swap request and reset back buffer ready flag
        s_swap_requested = false;
        s_back_buffer.ready = false;  // Back buffer needs to be reloaded
        s_back_buffer.first_frame_ready = false;  // Clear prefetch flag
        s_back_buffer.prefetch_pending = false;  // Clear prefetch pending flag
        
        xSemaphoreGive(s_buffer_mutex);
        
        ESP_LOGI(TAG, "Buffers swapped: front now playing index %zu", s_front_buffer.asset_index);
    }
}

// Initialize animation decoder and allocate buffers for a given animation buffer
static esp_err_t init_animation_decoder_for_buffer(animation_buffer_t *buf, asset_type_t type, const uint8_t *data, size_t size)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    
    animation_decoder_type_t decoder_type;
    if (type == ASSET_TYPE_WEBP) {
        decoder_type = ANIMATION_DECODER_TYPE_WEBP;
    } else if (type == ASSET_TYPE_GIF) {
        decoder_type = ANIMATION_DECODER_TYPE_GIF;
    } else if (type == ASSET_TYPE_PNG) {
        decoder_type = ANIMATION_DECODER_TYPE_PNG;
    } else if (type == ASSET_TYPE_JPEG) {
        decoder_type = ANIMATION_DECODER_TYPE_JPEG;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t err = animation_decoder_init(&buf->decoder, decoder_type, data, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize decoder");
        return err;
    }

    err = animation_decoder_get_info(buf->decoder, &buf->decoder_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get decoder info");
        animation_decoder_unload(&buf->decoder);
        return err;
    }

    const int canvas_w = (int)buf->decoder_info.canvas_width;
    const int canvas_h = (int)buf->decoder_info.canvas_height;
    buf->native_frame_size = (size_t)canvas_w * canvas_h * 4; // RGBA
    
    buf->native_frame_b1 = (uint8_t *)malloc(buf->native_frame_size);
    if (!buf->native_frame_b1) {
        ESP_LOGE(TAG, "Failed to allocate native frame buffer B1");
        animation_decoder_unload(&buf->decoder);
        return ESP_ERR_NO_MEM;
    }
    
    buf->native_frame_b2 = (uint8_t *)malloc(buf->native_frame_size);
    if (!buf->native_frame_b2) {
        ESP_LOGE(TAG, "Failed to allocate native frame buffer B2");
        free(buf->native_frame_b1);
        buf->native_frame_b1 = NULL;
        animation_decoder_unload(&buf->decoder);
        return ESP_ERR_NO_MEM;
    }
    
    buf->native_buffer_active = 0;
    
    const int target_w = EXAMPLE_LCD_H_RES;
    const int target_h = EXAMPLE_LCD_V_RES;
    
    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);
    
    buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc((size_t)target_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf->upscale_lookup_x) {
        ESP_LOGE(TAG, "Failed to allocate upscale lookup X");
        unload_animation_buffer(buf);
        return ESP_ERR_NO_MEM;
    }
    
    buf->upscale_lookup_y = (uint16_t *)heap_caps_malloc((size_t)target_h * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf->upscale_lookup_y) {
        ESP_LOGE(TAG, "Failed to allocate upscale lookup Y");
        unload_animation_buffer(buf);
        return ESP_ERR_NO_MEM;
    }
    
    for (int dst_x = 0; dst_x < target_w; ++dst_x) {
        int src_x = (dst_x * canvas_w) / target_w;
        if (src_x >= canvas_w) {
            src_x = canvas_w - 1;
        }
        buf->upscale_lookup_x[dst_x] = (uint16_t)src_x;
    }
    
    for (int dst_y = 0; dst_y < target_h; ++dst_y) {
        int src_y = (dst_y * canvas_h) / target_h;
        if (src_y >= canvas_h) {
            src_y = canvas_h - 1;
        }
        buf->upscale_lookup_y[dst_y] = (uint16_t)src_y;
    }
    
    buf->upscale_src_w = canvas_w;
    buf->upscale_src_h = canvas_h;
    buf->upscale_dst_w = target_w;
    buf->upscale_dst_h = target_h;

    return ESP_OK;
}

// Load animation file and initialize decoder into specified buffer
static esp_err_t load_animation_into_buffer(size_t asset_index, animation_buffer_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_sd_file_list.count == 0) {
        ESP_LOGE(TAG, "No animation files available");
        return ESP_ERR_NOT_FOUND;
    }

    if (asset_index >= s_sd_file_list.count) {
        ESP_LOGE(TAG, "Invalid asset index: %zu (max: %zu)", asset_index, s_sd_file_list.count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    // Unload previous animation in this buffer
    unload_animation_buffer(buf);

    const char *filename = s_sd_file_list.filenames[asset_index];
    const char *animations_dir = s_sd_file_list.animations_dir;
    asset_type_t type = s_sd_file_list.types[asset_index];
    
    if (!animations_dir) {
        ESP_LOGE(TAG, "Animations directory not set");
        return ESP_ERR_INVALID_STATE;
    }
    
    char filepath[512];
    int ret = snprintf(filepath, sizeof(filepath), "%s/%s", animations_dir, filename);
    if (ret < 0 || ret >= (int)sizeof(filepath)) {
        ESP_LOGE(TAG, "File path too long");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *file_data = NULL;
    size_t file_size = 0;
    esp_err_t err = load_animation_file_from_sd(filepath, &file_data, &file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load file from SD: %s", esp_err_to_name(err));
        // Mark file as unhealthy (file has issues)
        if (s_sd_file_list.health_flags && asset_index < s_sd_file_list.count) {
            s_sd_file_list.health_flags[asset_index] = false;
            ESP_LOGW(TAG, "Marked file '%s' (index %zu) as unhealthy due to load failure", filename, asset_index);
        }
        return err;
    }

    buf->file_data = file_data;
    buf->file_size = file_size;
    buf->type = type;
    buf->asset_index = asset_index;

    err = init_animation_decoder_for_buffer(buf, type, file_data, file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize animation decoder '%s': %s", filename, esp_err_to_name(err));
        // Mark file as unhealthy (file has issues)
        if (s_sd_file_list.health_flags && asset_index < s_sd_file_list.count) {
            s_sd_file_list.health_flags[asset_index] = false;
            ESP_LOGW(TAG, "Marked file '%s' (index %zu) as unhealthy due to decoder initialization failure", filename, asset_index);
        }
        free(file_data);
        buf->file_data = NULL;
        buf->file_size = 0;
        return err;
    }
    
    // Mark file as healthy (file is ok) since loading succeeded
    if (s_sd_file_list.health_flags && asset_index < s_sd_file_list.count) {
        s_sd_file_list.health_flags[asset_index] = true;
    }

    // Allocate prefetched frame buffer (LCD-sized)
    buf->prefetched_first_frame = (uint8_t *)malloc(s_frame_buffer_bytes);
    if (!buf->prefetched_first_frame) {
        ESP_LOGE(TAG, "Failed to allocate prefetched frame buffer");
        unload_animation_buffer(buf);
        return ESP_ERR_NO_MEM;
    }
    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;

    ESP_LOGI(TAG, "Loaded animation into buffer: %s (index %zu)", filename, asset_index);

    return ESP_OK;
}

// Pre-decode and upscale the first frame into the prefetched buffer
static esp_err_t prefetch_first_frame(animation_buffer_t *buf)
{
    if (!buf || !buf->decoder || !buf->prefetched_first_frame) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Decode frame 0 into native buffer
    uint8_t *decode_buffer = buf->native_frame_b1;
    esp_err_t err = animation_decoder_decode_next(buf->decoder, decode_buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode first frame for prefetch: %s", esp_err_to_name(err));
        return err;
    }
    
    // Get frame delay for the prefetched first frame
    uint32_t frame_delay_ms = 1;
    esp_err_t delay_err = animation_decoder_get_frame_delay(buf->decoder, &frame_delay_ms);
    if (delay_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get prefetch frame delay, using default");
        frame_delay_ms = 1;
    }
    buf->prefetched_first_frame_delay_ms = frame_delay_ms;
    
    // Upscale directly into prefetched buffer using buffer's lookup tables
    const uint8_t *src_for_upscale = decode_buffer;
    const int target_h = EXAMPLE_LCD_V_RES;
    const int dst_h = target_h;
    const int mid_row = dst_h / 2;
    
    // Set up upscale parameters
    s_upscale_src_buffer = src_for_upscale;
    s_upscale_dst_buffer = buf->prefetched_first_frame;
    s_upscale_lookup_x = buf->upscale_lookup_x;
    s_upscale_lookup_y = buf->upscale_lookup_y;
    s_upscale_src_w = buf->upscale_src_w;
    s_upscale_src_h = buf->upscale_src_h;
    s_upscale_main_task = xTaskGetCurrentTaskHandle();
    
    s_upscale_worker_top_done = false;
    s_upscale_worker_bottom_done = false;
    
    s_upscale_row_start_top = 0;
    s_upscale_row_end_top = mid_row;
    s_upscale_row_start_bottom = mid_row;
    s_upscale_row_end_bottom = dst_h;
    
    // Safety check: workers must exist for prefetch
    if (!s_upscale_worker_top || !s_upscale_worker_bottom) {
        ESP_LOGE(TAG, "Upscale workers not available for prefetch");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Memory barrier to ensure all shared variables are visible to worker cores
    MEMORY_BARRIER();
    
    // Notify workers to start processing
    if (s_upscale_worker_top) {
        xTaskNotify(s_upscale_worker_top, 1, eSetBits);
    }
    if (s_upscale_worker_bottom) {
        xTaskNotify(s_upscale_worker_bottom, 1, eSetBits);
    }
    
    // Wait for both workers to complete using proper notification API
    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;
    
    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            // Timeout - yield to allow idle task to reset watchdog
            taskYIELD();
        }
    }
    
    if (!s_upscale_worker_top_done || !s_upscale_worker_bottom_done) {
        ESP_LOGW(TAG, "Upscale workers may not have completed properly during prefetch");
        return ESP_FAIL;
    }
    
    // Memory barrier to ensure all worker writes are visible
    MEMORY_BARRIER();
    
    // Mark first frame as ready
    buf->first_frame_ready = true;
    
    // After decoding frame 0, decoder is positioned for frame 1
    // We don't reset - when render loop starts, it will use prefetched frame 0,
    // then decode frame 1 (which decoder is already positioned for)
    buf->decoder_at_frame_1 = true;
    
    ESP_LOGD(TAG, "Prefetched first frame for animation index %zu", buf->asset_index);
    
    return ESP_OK;
}

esp_err_t animation_player_init(esp_lcd_panel_handle_t display_handle,
                                 uint8_t **lcd_buffers,
                                 uint8_t buffer_count,
                                 size_t buffer_bytes,
                                 size_t row_stride_bytes)
{
    if (!display_handle || !lcd_buffers || buffer_count == 0 || buffer_bytes == 0 || row_stride_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_display_handle = display_handle;
    s_lcd_buffers = lcd_buffers;
    s_buffer_count = buffer_count;
    s_frame_buffer_bytes = buffer_bytes;
    s_frame_row_stride_bytes = row_stride_bytes;

    if (s_buffer_count > 1) {
        if (s_vsync_sem == NULL) {
            s_vsync_sem = xSemaphoreCreateBinary();
        }
        if (s_vsync_sem == NULL) {
            ESP_LOGE(TAG, "Failed to allocate VSYNC semaphore");
            return ESP_ERR_NO_MEM;
        }
        (void)xSemaphoreTake(s_vsync_sem, 0);
        xSemaphoreGive(s_vsync_sem);

        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_refresh_done = lcd_panel_refresh_done_cb,
        };
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(display_handle, &cbs, s_vsync_sem));
    } else if (s_vsync_sem) {
        vSemaphoreDelete(s_vsync_sem);
        s_vsync_sem = NULL;
        ESP_LOGW(TAG, "Single LCD frame buffer in use; tearing may occur");
    }

    ESP_LOGI(TAG, "Mounting SD card...");
    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(sd_err));
        return sd_err;
    }
    s_sd_mounted = true;

    const char *sd_root = BSP_SD_MOUNT_POINT;
    char *found_animations_dir = NULL;
    const char *preferred_dir = "/sdcard/animations";

    if (directory_has_animation_files(preferred_dir)) {
        found_animations_dir = strdup(preferred_dir);
        if (!found_animations_dir) {
            ESP_LOGE(TAG, "Failed to allocate preferred animations directory string");
            bsp_sdcard_unmount();
            s_sd_mounted = false;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Using preferred animations directory: %s", preferred_dir);
    } else {
        ESP_LOGI(TAG, "Preferred directory empty or missing, searching entire SD card starting from %s...", sd_root);
        esp_err_t find_err = find_animations_directory(sd_root, &found_animations_dir);
        if (find_err != ESP_OK || !found_animations_dir) {
            ESP_LOGE(TAG, "Failed to find directory with animation files: %s", esp_err_to_name(find_err));
            bsp_sdcard_unmount();
            s_sd_mounted = false;
            return (find_err == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Found animations directory: %s", found_animations_dir);

    esp_err_t enum_err = enumerate_animation_files(found_animations_dir);
    free(found_animations_dir);
    if (enum_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enumerate animation files: %s", esp_err_to_name(enum_err));
        bsp_sdcard_unmount();
        s_sd_mounted = false;
        return enum_err;
    }

    // ============================================================================
    // TEMPORARY DEBUG: Filter file list for testing
    // TODO: REMOVE THIS CALL AFTER DEBUGGING IS COMPLETE
    // ============================================================================
    // filter_file_list_for_debug();
    // ============================================================================

    if (s_sd_file_list.count == 0) {
        ESP_LOGE(TAG, "No animation files found");
        bsp_sdcard_unmount();
        s_sd_mounted = false;
        return ESP_ERR_NOT_FOUND;
    }

    // Initialize double buffer system
    s_buffer_mutex = xSemaphoreCreateMutex();
    if (!s_buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create buffer mutex");
        bsp_sdcard_unmount();
        s_sd_mounted = false;
        return ESP_ERR_NO_MEM;
    }
    
    s_loader_sem = xSemaphoreCreateBinary();
    if (!s_loader_sem) {
        ESP_LOGE(TAG, "Failed to create loader semaphore");
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        bsp_sdcard_unmount();
        s_sd_mounted = false;
        return ESP_ERR_NO_MEM;
    }

    // Initialize buffers to zero
    memset(&s_front_buffer, 0, sizeof(s_front_buffer));
    memset(&s_back_buffer, 0, sizeof(s_back_buffer));

    // Load the first healthy animation from the randomized list into front buffer synchronously
    size_t start_index = 0;
    // Find first healthy file
    if (s_sd_file_list.health_flags) {
        bool found_healthy = false;
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            if (s_sd_file_list.health_flags[i]) {
                start_index = i;
                found_healthy = true;
                break;
            }
        }
        if (!found_healthy) {
            ESP_LOGE(TAG, "No healthy animation files available at startup");
            vSemaphoreDelete(s_loader_sem);
            s_loader_sem = NULL;
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            bsp_sdcard_unmount();
            s_sd_mounted = false;
            return ESP_ERR_NOT_FOUND;
        }
    }
    
    esp_err_t load_err = load_animation_into_buffer(start_index, &s_front_buffer);
    if (load_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load animation at index %zu, trying other healthy files...", start_index);
        // Try other healthy animations sequentially as fallback
        bool found_any = false;
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            // Skip if not healthy (if health flags available)
            if (s_sd_file_list.health_flags && !s_sd_file_list.health_flags[i]) {
                continue;
            }
            if (i == start_index) {
                continue;  // Already tried this one
            }
            load_err = load_animation_into_buffer(i, &s_front_buffer);
            if (load_err == ESP_OK) {
                ESP_LOGI(TAG, "Successfully loaded animation at index %zu", i);
                found_any = true;
                break;
            }
        }
        if (!found_any || load_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load any healthy animation file");
            vSemaphoreDelete(s_loader_sem);
            s_loader_sem = NULL;
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            bsp_sdcard_unmount();
            s_sd_mounted = false;
            return load_err;
        }
    } else {
        ESP_LOGI(TAG, "Loaded animation at index %zu to start playback", start_index);
    }
    
    // Create upscale workers BEFORE prefetch (prefetch needs them)
    if (s_upscale_worker_top == NULL) {
        const BaseType_t worker_top_created = xTaskCreatePinnedToCore(
            upscale_worker_top_task,
            "upscale_top",
            2048,
            NULL,
            CONFIG_P3A_RENDER_TASK_PRIORITY,
            &s_upscale_worker_top,
            0
        );
        
        if (worker_top_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create top upscale worker task");
            unload_animation_buffer(&s_front_buffer);
            vSemaphoreDelete(s_loader_sem);
            s_loader_sem = NULL;
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            bsp_sdcard_unmount();
            s_sd_mounted = false;
            return ESP_FAIL;
        }
    }
    
    if (s_upscale_worker_bottom == NULL) {
        const BaseType_t worker_bottom_created = xTaskCreatePinnedToCore(
            upscale_worker_bottom_task,
            "upscale_bottom",
            2048,
            NULL,
            CONFIG_P3A_RENDER_TASK_PRIORITY,
            &s_upscale_worker_bottom,
            1
        );
        
        if (worker_bottom_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create bottom upscale worker task");
            unload_animation_buffer(&s_front_buffer);
            vSemaphoreDelete(s_loader_sem);
            s_loader_sem = NULL;
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            bsp_sdcard_unmount();
            s_sd_mounted = false;
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "Created parallel upscaling worker tasks (CPU0: top, CPU1: bottom)");
    
    // Prefetch first frame of front buffer (now that workers exist)
    // This is done synchronously during init, so it's safe
    esp_err_t prefetch_err = prefetch_first_frame(&s_front_buffer);
    if (prefetch_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to prefetch first frame during init: %s", esp_err_to_name(prefetch_err));
    }
    
    // Mark front buffer as ready
    s_front_buffer.ready = true;
    s_front_buffer.prefetch_pending = false;
    
    // Create loader task (back buffer will remain empty until swap gesture)
    const BaseType_t loader_created = xTaskCreate(
        animation_loader_task,
        "anim_loader",
        4096,
        NULL,
        CONFIG_P3A_RENDER_TASK_PRIORITY - 1,  // Lower priority than render task
        &s_loader_task
    );
    
    if (loader_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create loader task");
        unload_animation_buffer(&s_front_buffer);
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        bsp_sdcard_unmount();
        s_sd_mounted = false;
        return ESP_FAIL;
    }
    
    // Back buffer starts empty - will be loaded on-demand when swap gesture occurs

    return ESP_OK;
}

esp_err_t animation_player_load_asset(const char *filepath)
{
    (void)filepath; // Not used in double buffer system
    // Animation loading is handled by animation_player_cycle_animation()
    return ESP_ERR_NOT_SUPPORTED;
}

void animation_player_set_paused(bool paused)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        bool changed = (s_anim_paused != paused);
        s_anim_paused = paused;
        xSemaphoreGive(s_buffer_mutex);
        
        if (changed) {
            ESP_LOGI(TAG, "Animation %s", paused ? "paused" : "resumed");
        }
    }
}

void animation_player_toggle_pause(void)
{
    bool paused;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_anim_paused = !s_anim_paused;
        paused = s_anim_paused;
        xSemaphoreGive(s_buffer_mutex);
        
        ESP_LOGI(TAG, "Animation %s", paused ? "paused" : "resumed");
    }
}

bool animation_player_is_paused(void)
{
    bool paused = false;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        paused = s_anim_paused;
        xSemaphoreGive(s_buffer_mutex);
    }
    return paused;
}

void animation_player_cycle_animation(bool forward)
{
    if (is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SD card is exported over USB");
        return;
    }

    if (s_sd_file_list.count == 0) {
        ESP_LOGW(TAG, "No animations available to cycle");
        return;
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // If swap is already in progress (swap requested, loader busy, or prefetch pending), ignore
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return;
        }
        
        // Compute next or previous animation index on demand
        size_t current_index = s_front_buffer.ready ? s_front_buffer.asset_index : 0;
        size_t target_index = forward ? get_next_asset_index(current_index) : get_previous_asset_index(current_index);
        
        // Check if no healthy files are available (target_index == current_index means no healthy file found)
        if (target_index == current_index && s_sd_file_list.health_flags) {
            // Check if there are any healthy files at all
            bool any_healthy = false;
            for (size_t i = 0; i < s_sd_file_list.count; i++) {
                if (s_sd_file_list.health_flags[i]) {
                    any_healthy = true;
                    break;
                }
            }
            if (!any_healthy) {
                ESP_LOGW(TAG, "No healthy animation files available. Cannot cycle animation.");
                xSemaphoreGive(s_buffer_mutex);
                return;
            }
            // If we have healthy files but target_index == current_index, it means current is the only healthy one
            // This is fine, we can still try to swap (though it will likely fail if current is already loaded)
        }
        
        // Set swap requested and queue loader with target index
        s_next_asset_index = target_index;
        s_swap_requested = true;
        
        xSemaphoreGive(s_buffer_mutex);
        
        // Trigger loader task to load target animation
        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }
        
        ESP_LOGI(TAG, "Queued animation load to '%s' (index %zu)", 
                 s_sd_file_list.filenames[target_index], target_index);
    }
}

esp_err_t animation_player_begin_sd_export(void)
{
    if (is_sd_export_locked()) {
        return ESP_OK;
    }

    wait_for_loader_idle();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_export_active = true;
        s_swap_requested = false;
        s_back_buffer.prefetch_pending = false;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_export_active = true;
    }

    ESP_LOGI(TAG, "SD card exported to USB host");
    return ESP_OK;
}

esp_err_t animation_player_end_sd_export(void)
{
    if (!is_sd_export_locked()) {
        return ESP_OK;
    }

    esp_err_t refresh_err = refresh_animation_file_list();
    if (refresh_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to refresh animation list after SD remount: %s", esp_err_to_name(refresh_err));
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_export_active = false;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_export_active = false;
    }

    ESP_LOGI(TAG, "SD card returned to local control");
    return refresh_err;
}

bool animation_player_is_sd_export_locked(void)
{
    return is_sd_export_locked();
}

esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len)
{
    if (!pixel_data || pixel_len < PICO8_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_pico8_resources();
    if (err != ESP_OK) {
        return err;
    }

    if (palette_rgb && palette_len >= (PICO8_PALETTE_COLORS * 3)) {
        for (int i = 0; i < PICO8_PALETTE_COLORS; ++i) {
            size_t idx = (size_t)i * 3;
            s_pico8_palette[i].r = palette_rgb[idx + 0];
            s_pico8_palette[i].g = palette_rgb[idx + 1];
            s_pico8_palette[i].b = palette_rgb[idx + 2];
        }
    }

    const uint8_t target_index = s_pico8_decode_index & 0x01;
    uint8_t *target = s_pico8_frame_buffers[target_index];
    if (!target) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t total_pixels = (size_t)PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT;
    size_t pixel_cursor = 0;
    for (size_t i = 0; i < PICO8_FRAME_BYTES && i < pixel_len; ++i) {
        uint8_t packed = pixel_data[i];
        uint8_t low_idx = packed & 0x0F;
        uint8_t high_idx = (packed >> 4) & 0x0F;

        pico8_color_t low_color = s_pico8_palette[low_idx];
        pico8_color_t high_color = s_pico8_palette[high_idx];

        if (pixel_cursor < total_pixels) {
            uint8_t *dst = target + pixel_cursor * 4;
            dst[0] = low_color.r;
            dst[1] = low_color.g;
            dst[2] = low_color.b;
            dst[3] = 0xFF;
            pixel_cursor++;
        }

        if (pixel_cursor < total_pixels) {
            uint8_t *dst = target + pixel_cursor * 4;
            dst[0] = high_color.r;
            dst[1] = high_color.g;
            dst[2] = high_color.b;
            dst[3] = 0xFF;
            pixel_cursor++;
        }
    }

    const int64_t now = esp_timer_get_time();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_pico8_display_index = target_index;
        s_pico8_decode_index = target_index ^ 1;
        s_pico8_frame_ready = true;
        s_pico8_override_active = true;
        s_pico8_last_frame_time_us = now;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_pico8_display_index = target_index;
        s_pico8_decode_index = target_index ^ 1;
        s_pico8_frame_ready = true;
        s_pico8_override_active = true;
        s_pico8_last_frame_time_us = now;
    }

    return ESP_OK;
}

esp_err_t animation_player_start(void)
{
    if (s_anim_task == NULL) {
        const BaseType_t created = xTaskCreate(lcd_animation_task, "lcd_anim", 4096, NULL,
                                               CONFIG_P3A_RENDER_TASK_PRIORITY, &s_anim_task);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to start LCD animation task");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

void animation_player_deinit(void)
{
    release_pico8_resources();
    s_sd_export_active = false;

    // Stop loader task
    if (s_loader_task) {
        vTaskDelete(s_loader_task);
        s_loader_task = NULL;
    }
    
    // Unload both buffers
    unload_animation_buffer(&s_front_buffer);
    unload_animation_buffer(&s_back_buffer);
    
    // Clean up synchronization primitives
    if (s_loader_sem) {
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
    }
    
    if (s_buffer_mutex) {
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
    }
    
    free_sd_file_list();
    if (s_sd_mounted) {
        bsp_sdcard_unmount();
        s_sd_mounted = false;
    }
}

size_t animation_player_get_current_index(void)
{
    size_t current_index = SIZE_MAX; // Use SIZE_MAX to indicate "no animation playing"
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_front_buffer.ready) {
            current_index = s_front_buffer.asset_index;
        }
        xSemaphoreGive(s_buffer_mutex);
    }
    return current_index;
}

esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index)
{
    if (is_sd_export_locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!filename || !animations_dir) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate file extension
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        return ESP_ERR_INVALID_ARG;
    }
    ext++;
    bool valid_ext = false;
    if (strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0) {
        valid_ext = true;
    }
    if (!valid_ext) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if animations_dir matches current directory
    if (s_sd_file_list.animations_dir && strcmp(s_sd_file_list.animations_dir, animations_dir) != 0) {
        ESP_LOGW(TAG, "Adding file from different directory, updating animations_dir");
        free(s_sd_file_list.animations_dir);
        size_t dir_len = strlen(animations_dir);
        s_sd_file_list.animations_dir = (char *)malloc(dir_len + 1);
        if (!s_sd_file_list.animations_dir) {
            return ESP_ERR_NO_MEM;
        }
        strcpy(s_sd_file_list.animations_dir, animations_dir);
    } else if (!s_sd_file_list.animations_dir) {
        size_t dir_len = strlen(animations_dir);
        s_sd_file_list.animations_dir = (char *)malloc(dir_len + 1);
        if (!s_sd_file_list.animations_dir) {
            return ESP_ERR_NO_MEM;
        }
        strcpy(s_sd_file_list.animations_dir, animations_dir);
    }
    
    // Check if file already exists in list
    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        if (s_sd_file_list.filenames[i] && strcmp(s_sd_file_list.filenames[i], filename) == 0) {
            ESP_LOGW(TAG, "File %s already in list at index %zu", filename, i);
            if (out_index) {
                *out_index = i;
            }
            return ESP_OK; // Already exists, return success
        }
    }
    
    // Allocate new arrays with one more entry
    size_t new_count = s_sd_file_list.count + 1;
    char **new_filenames = (char **)realloc(s_sd_file_list.filenames, new_count * sizeof(char *));
    if (!new_filenames) {
        return ESP_ERR_NO_MEM;
    }
    
    asset_type_t *new_types = (asset_type_t *)realloc(s_sd_file_list.types, new_count * sizeof(asset_type_t));
    if (!new_types) {
        free(new_filenames);
        return ESP_ERR_NO_MEM;
    }
    
    bool *new_health_flags = NULL;
    if (s_sd_file_list.health_flags) {
        new_health_flags = (bool *)realloc(s_sd_file_list.health_flags, new_count * sizeof(bool));
        if (!new_health_flags) {
            free(new_filenames);
            free(new_types);
            return ESP_ERR_NO_MEM;
        }
    } else {
        new_health_flags = (bool *)malloc(new_count * sizeof(bool));
        if (!new_health_flags) {
            free(new_filenames);
            free(new_types);
            return ESP_ERR_NO_MEM;
        }
        // Initialize all existing entries as healthy
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            new_health_flags[i] = true;
        }
    }
    
    // Allocate filename string
    size_t filename_len = strlen(filename);
    new_filenames[s_sd_file_list.count] = (char *)malloc(filename_len + 1);
    if (!new_filenames[s_sd_file_list.count]) {
        free(new_filenames);
        free(new_types);
        free(new_health_flags);
        return ESP_ERR_NO_MEM;
    }
    strcpy(new_filenames[s_sd_file_list.count], filename);
    new_types[s_sd_file_list.count] = get_asset_type(filename);
    new_health_flags[s_sd_file_list.count] = true; // New file is healthy
    
    // Update list
    s_sd_file_list.filenames = new_filenames;
    s_sd_file_list.types = new_types;
    s_sd_file_list.health_flags = new_health_flags;
    s_sd_file_list.count = new_count;
    
    // Determine insertion index
    size_t insert_index;
    if (s_sd_file_list.count == 1) {
        // First file, insert at index 0
        insert_index = 0;
    } else {
        // Insert immediately after insert_after_index
        // If insert_after_index is SIZE_MAX (nothing playing), insert at index 0
        if (insert_after_index == SIZE_MAX) {
            insert_index = 0;
            // Shift all existing elements one position to the right
            for (size_t i = s_sd_file_list.count - 1; i > 0; i--) {
                // Swap with previous element
                char *temp_filename = s_sd_file_list.filenames[i];
                s_sd_file_list.filenames[i] = s_sd_file_list.filenames[i - 1];
                s_sd_file_list.filenames[i - 1] = temp_filename;
                
                asset_type_t temp_type = s_sd_file_list.types[i];
                s_sd_file_list.types[i] = s_sd_file_list.types[i - 1];
                s_sd_file_list.types[i - 1] = temp_type;
                
                if (s_sd_file_list.health_flags) {
                    bool temp_health = s_sd_file_list.health_flags[i];
                    s_sd_file_list.health_flags[i] = s_sd_file_list.health_flags[i - 1];
                    s_sd_file_list.health_flags[i - 1] = temp_health;
                }
            }
        } else if (insert_after_index >= s_sd_file_list.count - 1) {
            // Insert at end (already there, no need to move)
            insert_index = s_sd_file_list.count - 1;
        } else {
            // Insert after insert_after_index, so new index is insert_after_index + 1
            insert_index = insert_after_index + 1;
            
            // Shift elements from insert_index to end-1 one position to the right
            // The new file is currently at count-1, we need to move it to insert_index
            for (size_t i = s_sd_file_list.count - 1; i > insert_index; i--) {
                // Swap with previous element
                char *temp_filename = s_sd_file_list.filenames[i];
                s_sd_file_list.filenames[i] = s_sd_file_list.filenames[i - 1];
                s_sd_file_list.filenames[i - 1] = temp_filename;
                
                asset_type_t temp_type = s_sd_file_list.types[i];
                s_sd_file_list.types[i] = s_sd_file_list.types[i - 1];
                s_sd_file_list.types[i - 1] = temp_type;
                
                if (s_sd_file_list.health_flags) {
                    bool temp_health = s_sd_file_list.health_flags[i];
                    s_sd_file_list.health_flags[i] = s_sd_file_list.health_flags[i - 1];
                    s_sd_file_list.health_flags[i - 1] = temp_health;
                }
            }
        }
    }
    
    if (out_index) {
        *out_index = insert_index;
    }
    
    ESP_LOGI(TAG, "Added file %s to animation list at index %zu (inserted after index %zu, total: %zu)", 
             filename, insert_index, insert_after_index, s_sd_file_list.count);
    return ESP_OK;
}

esp_err_t animation_player_swap_to_index(size_t index)
{
    if (is_sd_export_locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sd_file_list.count == 0) {
        ESP_LOGW(TAG, "No animations available to swap");
        return ESP_ERR_NOT_FOUND;
    }
    
    if (index >= s_sd_file_list.count) {
        ESP_LOGE(TAG, "Invalid index: %zu (max: %zu)", index, s_sd_file_list.count - 1);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if file is healthy
    if (s_sd_file_list.health_flags && !s_sd_file_list.health_flags[index]) {
        ESP_LOGW(TAG, "File at index %zu is marked as unhealthy", index);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // If swap is already in progress, ignore
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        
        // Set swap requested and queue loader with target index
        s_next_asset_index = index;
        s_swap_requested = true;
        
        xSemaphoreGive(s_buffer_mutex);
        
        // Trigger loader task to load target animation
        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }
        
        ESP_LOGI(TAG, "Queued animation load to '%s' (index %zu)", 
                 s_sd_file_list.filenames[index], index);
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

