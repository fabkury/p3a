#include "video_player.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "sdkconfig.h"
#include "webp/demux.h"
#include "webp/decode.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "p3a_hal/display.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "gif_decoder.h"

static const char *TAG = "video_player";

// Configuration
#define VIDEO_W 720
#define VIDEO_H 720
#ifndef VIDEO_TILE_H
#define VIDEO_TILE_H 80   // Stripe height (tunable: 60-120)
#endif

// Video player state
typedef struct {
    // Stripe buffers (ping-pong)
    uint8_t *stripeA;
    uint8_t *stripeB;
    size_t stripe_size;
    size_t stripe_height;
    
    // Panel handle
    esp_lcd_panel_handle_t panel_handle;
    lv_display_t *lvgl_display;
    SemaphoreHandle_t panel_done_sem;
    SemaphoreHandle_t lvgl_trans_sem;
    bool lvgl_avoid_tearing;
    bool panel_callbacks_registered;
    
    // Playback state
    bool is_playing;
    bool is_paused;
    bool should_loop;
    bool should_stop;
    bool keep_bypass_on_stop;
    
    // Animation format
    anim_format_t format;
    
    // WebP decoder state
    WebPAnimDecoder *webp_decoder;
    WebPAnimInfo anim_info;
    uint8_t *webp_data;
    size_t webp_size;
    int *frame_delays;
    size_t frame_count;
    size_t frame_index;
    
    // GIF decoder state
    gif_decoder_state_t *gif_decoder;
    uint8_t *gif_frame_buffer;  // Full frame RGB888 buffer for GIF
    size_t gif_frame_buffer_size;
    char *gif_file_path;
    uint32_t gif_frame_counter;
    
    // Source animation dimensions (may differ from VIDEO_W/VIDEO_H)
    int anim_width;
    int anim_height;
    
    // Precomputed scaling indices for performance optimization
    int *x_index_map;      // Precomputed X source indices for each destination X
    int *y_index_map;      // Precomputed Y source indices for each destination Y
    size_t x_index_map_size;
    size_t y_index_map_size;
    bool needs_scaling;    // True if animation dimensions differ from display
    
    // DMA state
    volatile bool dma_busy[2];  // One per stripe buffer
    SemaphoreHandle_t dma_done_sem[2];
    
    // Task handle
    TaskHandle_t playback_task;
    
    // Statistics
    float current_fps;
    float decode_ms_per_stripe;
    float dma_ms_per_stripe;
    float frame_ms_total;
    uint32_t frame_count_stats;
    int64_t stats_start_time_us;
    
    // LVGL suspend state
    bool lvgl_suspended;
} video_player_state_t;

static video_player_state_t s_player = {0};

// Forward declarations
static void playback_task(void *pvParameters);
static esp_err_t enter_lvgl_bypass_mode(void);
static esp_err_t exit_lvgl_bypass_mode(void);
static esp_err_t send_stripe_dma(int stripe_idx, int y, int h, uint8_t *buf);
static bool panel_color_trans_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);
static bool panel_refresh_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);
static void cleanup_webp_decoder(void);
static void cleanup_gif_decoder(void);
static anim_format_t detect_format_from_path(const char *file_path);
static anim_format_t detect_format_from_header(const uint8_t *data, size_t size);

static void cleanup_webp_decoder(void)
{
    if (s_player.webp_decoder) {
        WebPAnimDecoderDelete(s_player.webp_decoder);
        s_player.webp_decoder = NULL;
    }
    if (s_player.webp_data) {
        free(s_player.webp_data);
        s_player.webp_data = NULL;
    }
    if (s_player.frame_delays) {
        free(s_player.frame_delays);
        s_player.frame_delays = NULL;
    }
    s_player.webp_size = 0;
    s_player.frame_count = 0;
    s_player.frame_index = 0;
}

static void cleanup_gif_decoder(void)
{
    if (s_player.gif_decoder) {
        gif_decoder_close(s_player.gif_decoder);
        free(s_player.gif_decoder);
        s_player.gif_decoder = NULL;
    }
    if (s_player.gif_frame_buffer) {
        free(s_player.gif_frame_buffer);
        s_player.gif_frame_buffer = NULL;
    }
    if (s_player.gif_file_path) {
        free(s_player.gif_file_path);
        s_player.gif_file_path = NULL;
    }
    s_player.gif_frame_buffer_size = 0;
}

static anim_format_t detect_format_from_path(const char *file_path)
{
    if (!file_path) {
        return ANIM_FORMAT_UNKNOWN;
    }
    
    const char *ext = strrchr(file_path, '.');
    if (!ext) {
        return ANIM_FORMAT_UNKNOWN;
    }
    
    if (strcasecmp(ext, ".webp") == 0) {
        return ANIM_FORMAT_WEBP;
    } else if (strcasecmp(ext, ".gif") == 0) {
        return ANIM_FORMAT_GIF;
    }
    
    return ANIM_FORMAT_UNKNOWN;
}

static anim_format_t detect_format_from_header(const uint8_t *data, size_t size)
{
    if (!data || size < 12) {
        return ANIM_FORMAT_UNKNOWN;
    }
    
    // Check for WebP signature: RIFF....WEBP or RIFF....VP8
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0) {
        if (memcmp(data + 8, "WEBP", 4) == 0) {
            return ANIM_FORMAT_WEBP;
        }
    }
    
    // Check for GIF signature: GIF87a or GIF89a
    if (size >= 6 && memcmp(data, "GIF87", 5) == 0) {
        return ANIM_FORMAT_GIF;
    }
    if (size >= 6 && memcmp(data, "GIF89", 5) == 0) {
        return ANIM_FORMAT_GIF;
    }
    
    return ANIM_FORMAT_UNKNOWN;
}

static bool panel_color_trans_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    video_player_state_t *state = (video_player_state_t *)user_ctx;
    if (!state) {
        return false;
    }

    BaseType_t task_woken = pdFALSE;

    if (state->panel_done_sem) {
        xSemaphoreGiveFromISR(state->panel_done_sem, &task_woken);
    }

    if (!state->lvgl_avoid_tearing && state->lvgl_display) {
        lv_disp_flush_ready(state->lvgl_display);
    }

    return task_woken == pdTRUE;
}

static bool panel_refresh_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    video_player_state_t *state = (video_player_state_t *)user_ctx;
    if (!state) {
        return false;
    }

    BaseType_t task_woken = pdFALSE;

    if (state->panel_done_sem) {
        xSemaphoreGiveFromISR(state->panel_done_sem, &task_woken);
    }

    if (state->lvgl_trans_sem) {
        xSemaphoreGiveFromISR(state->lvgl_trans_sem, &task_woken);
    }

    return task_woken == pdTRUE;
}

esp_err_t video_player_init(void)
{
    if (s_player.stripeA != NULL) {
        ESP_LOGW(TAG, "Video player already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing video player...");
    ESP_LOGI(TAG, "Resolution: %dx%d, Stripe height: %d", VIDEO_W, VIDEO_H, VIDEO_TILE_H);
    
    // Calculate stripe size
    const size_t cache_line_size = 64;
    size_t stripe_height = VIDEO_TILE_H;
    const size_t min_height = 16;
    size_t aligned_size = 0;

    while (stripe_height >= min_height) {
        const size_t stripe_size = (size_t)VIDEO_W * stripe_height * 3;
        aligned_size = ((stripe_size + cache_line_size - 1) / cache_line_size) * cache_line_size;

        ESP_LOGI(TAG, "Allocating stripe buffers: %zu bytes each (height=%zu)",
                 aligned_size, stripe_height);

        uint8_t *stripeA = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

        if (!stripeA) {
            ESP_LOGW(TAG, "Failed to allocate stripe buffer at height=%zu, retrying with half height",
                     stripe_height);
            stripe_height /= 2;
            continue;
        }

        uint8_t *stripeB = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

        if (!stripeB) {
            ESP_LOGW(TAG, "Failed to allocate second stripe at height=%zu, retrying with half height",
                     stripe_height);
            free(stripeA);
            stripe_height /= 2;
            continue;
        }

        s_player.stripe_height = stripe_height;
        s_player.stripe_size = (size_t)VIDEO_W * stripe_height * 3;
        s_player.stripeA = stripeA;
        s_player.stripeB = stripeB;
        break;
    }

    if (!s_player.stripeA || !s_player.stripeB) {
        ESP_LOGE(TAG, "Unable to allocate stripe buffers even after reducing height");
        s_player.stripeA = NULL;
        s_player.stripeB = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Create DMA done semaphores
    s_player.dma_done_sem[0] = xSemaphoreCreateBinary();
    s_player.dma_done_sem[1] = xSemaphoreCreateBinary();
    s_player.panel_done_sem = xSemaphoreCreateBinary();
    
    if (!s_player.dma_done_sem[0] || !s_player.dma_done_sem[1] || !s_player.panel_done_sem) {
        ESP_LOGE(TAG, "Failed to create DMA/panel semaphores");
        free(s_player.stripeA);
        free(s_player.stripeB);
        s_player.stripeA = NULL;
        s_player.stripeB = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize DMA state
    s_player.dma_busy[0] = false;
    s_player.dma_busy[1] = false;
    
    s_player.is_playing = false;
    s_player.should_stop = false;
    s_player.keep_bypass_on_stop = false;
    s_player.lvgl_suspended = false;
    s_player.lvgl_trans_sem = NULL;
    s_player.lvgl_avoid_tearing = false;
    s_player.panel_callbacks_registered = false;
    
    // Initialize scaling index maps to NULL
    s_player.x_index_map = NULL;
    s_player.y_index_map = NULL;
    s_player.x_index_map_size = 0;
    s_player.y_index_map_size = 0;
    s_player.needs_scaling = false;
    
    ESP_LOGI(TAG, "Video player initialized: stripe size=%zu bytes, height=%zu lines", 
             s_player.stripe_size, s_player.stripe_height);
    ESP_LOGI(TAG, "Total internal SRAM used: %zu bytes (2 stripes)", aligned_size * 2);
    
    return ESP_OK;
}

static esp_err_t enter_lvgl_bypass_mode(void)
{
    if (s_player.lvgl_suspended) {
        return ESP_OK;  // Already suspended
    }
    
    ESP_LOGI(TAG, "Entering LVGL bypass mode...");
    
    // Get LVGL display handle
    s_player.lvgl_display = p3a_hal_display_get_handle();
    if (!s_player.lvgl_display) {
        ESP_LOGE(TAG, "LVGL display not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // First, lock LVGL to ensure no operations are in progress
    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to lock LVGL mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    // Wait for LVGL to finish any pending flush operations
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Get panel handle from LVGL display context
    // Access private context via lv_display_get_driver_data
    typedef struct {
        uint8_t disp_type;
        void *io_handle;
        esp_lcd_panel_handle_t panel_handle;
        void *control_handle;
        lvgl_port_rotation_cfg_t rotation;
        lv_color_t *draw_buffs[3];
        uint8_t *oled_buffer;
        lv_display_t *disp_drv;
        lv_display_rotation_t current_rotation;
        SemaphoreHandle_t trans_sem;
    } lvgl_port_display_ctx_t;
    
    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)lv_display_get_driver_data(s_player.lvgl_display);
    if (!disp_ctx || !disp_ctx->panel_handle) {
        ESP_LOGE(TAG, "Failed to get panel handle from LVGL display");
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.panel_handle = disp_ctx->panel_handle;
    s_player.lvgl_trans_sem = disp_ctx->trans_sem;
    s_player.lvgl_avoid_tearing = (disp_ctx->trans_sem != NULL);
    ESP_LOGI(TAG, "Got panel handle: %p", (void *)s_player.panel_handle);

    // Register panel callbacks (override LVGL defaults, but replicate their behavior)
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_color_trans_done = panel_color_trans_done_cb,
        .on_refresh_done = panel_refresh_done_cb,
    };
    esp_err_t cb_ret = esp_lcd_dpi_panel_register_event_callbacks(s_player.panel_handle, &callbacks, &s_player);
    if (cb_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register panel callbacks: %s", esp_err_to_name(cb_ret));
    } else {
        s_player.panel_callbacks_registered = true;
    }
    
    // Wait longer for LVGL to finish all pending operations
    // The panel may still be busy from LVGL's last flush
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Now suspend LVGL tick timer
    if (lvgl_port_stop() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop LVGL tick timer");
    }
    
    // Note: We keep the lock during playback to prevent LVGL from interfering
    s_player.lvgl_suspended = true;
    
    ESP_LOGI(TAG, "LVGL bypass mode entered");
    return ESP_OK;
}

static esp_err_t exit_lvgl_bypass_mode(void)
{
    if (!s_player.lvgl_suspended) {
        return ESP_OK;  // Already resumed
    }
    
    ESP_LOGI(TAG, "Exiting LVGL bypass mode...");
    
    // Wait for any in-flight DMA to finish
    while (s_player.dma_busy[0] || s_player.dma_busy[1]) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // Release LVGL mutex
    bsp_display_unlock();
    
    // Resume LVGL tick timer
    if (lvgl_port_resume() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to resume LVGL tick timer");
    }
    
    s_player.lvgl_suspended = false;
    s_player.panel_handle = NULL;
    
    ESP_LOGI(TAG, "LVGL bypass mode exited");
    return ESP_OK;
}

// Free precomputed scaling index maps
static void free_index_maps(void)
{
    if (s_player.x_index_map) {
        free(s_player.x_index_map);
        s_player.x_index_map = NULL;
    }
    if (s_player.y_index_map) {
        free(s_player.y_index_map);
        s_player.y_index_map = NULL;
    }
    s_player.x_index_map_size = 0;
    s_player.y_index_map_size = 0;
}

// Precompute scaling index maps for performance optimization
static esp_err_t compute_index_maps(int src_w, int src_h, int dst_w, int dst_h)
{
    // Free existing maps if any (noop when not allocated)
    free_index_maps();

    s_player.needs_scaling = !(src_w == dst_w && src_h == dst_h);
    // Lookup tables are currently disabled for reliability; scaling is handled on-the-fly.
    return ESP_OK;
}

// Convert RGB888 with nearest neighbor scaling if needed (stripe-aware)
static void convert_rgba_to_rgb888_stripe(const uint8_t *src_rgba, int src_w, int src_h,
                                          uint8_t *dst_rgb, int dst_w, int dst_h,
                                          int dst_y, int dst_h_stripe)
{
    if (!src_rgba || !dst_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || dst_h_stripe <= 0) {
        return;
    }

    const size_t src_stride = (size_t)src_w * 4;
    const size_t dst_stride = (size_t)dst_w * 3;

    // Fast path: 1:1 copy (no scaling)
    if (src_w == dst_w && src_h == dst_h) {
        for (int y = 0; y < dst_h_stripe; ++y) {
            const uint8_t *src_row = src_rgba + (size_t)(dst_y + y) * src_stride;
            uint8_t *dst_row = dst_rgb + (size_t)y * dst_stride;
            for (int x = 0; x < dst_w; ++x) {
                const uint8_t *src_px = src_row + (size_t)x * 4;
                uint8_t *dst_px = dst_row + (size_t)x * 3;
                // WebP outputs RGBA, but display expects BGR888 (swap R and B)
                dst_px[0] = src_px[2];  // B
                dst_px[1] = src_px[1];  // G
                dst_px[2] = src_px[0];  // R
            }
        }
        return;
    }

    const uint32_t y_step = (uint32_t)(((uint64_t)src_h << 16) / (uint32_t)dst_h);
    uint32_t src_y_acc = (uint32_t)(((uint64_t)dst_y * (uint64_t)src_h << 16) / (uint32_t)dst_h);
    const uint32_t x_step = (uint32_t)(((uint64_t)src_w << 16) / (uint32_t)dst_w);

    for (int y = 0; y < dst_h_stripe; ++y) {
        int src_y = (int)(src_y_acc >> 16);
        if (src_y >= src_h) {
            src_y = src_h - 1;
        }
        if (src_y < 0) {
            src_y = 0;
        }

        const uint8_t *src_row = src_rgba + (size_t)src_y * src_stride;
        uint8_t *dst_row = dst_rgb + (size_t)y * dst_stride;

        uint32_t src_x_acc = 0;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = (int)(src_x_acc >> 16);
            if (src_x >= src_w) {
                src_x = src_w - 1;
            }

            const uint8_t *src_px = src_row + (size_t)src_x * 4;
            uint8_t *dst_px = dst_row + (size_t)x * 3;
            // WebP outputs RGBA, but display expects BGR888 (swap R and B)
            dst_px[0] = src_px[2];  // B
            dst_px[1] = src_px[1];  // G
            dst_px[2] = src_px[0];  // R

            src_x_acc += x_step;
        }

        src_y_acc += y_step;
    }
}

static esp_err_t send_stripe_dma(int stripe_idx, int y, int h, uint8_t *buf)
{
    if (!s_player.panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Wait for previous DMA on this stripe buffer to finish
    while (s_player.dma_busy[stripe_idx]) {
        if (xSemaphoreTake(s_player.dma_done_sem[stripe_idx], pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "DMA timeout waiting for stripe %d", stripe_idx);
            return ESP_ERR_TIMEOUT;
        }
    }
    
    // Ensure cache is synchronized for DMA (buffer is in internal memory, so this is needed)
    uintptr_t aligned_addr = (uintptr_t)buf & ~(uintptr_t)0x3F;  // 64-byte alignment
    size_t aligned_size = ((VIDEO_W * h * 3 + 0x3F) / 0x40) * 0x40;
    esp_cache_msync((void *)aligned_addr, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    
    // Mark DMA as busy before calling draw_bitmap
    s_player.dma_busy[stripe_idx] = true;
    
    // Clear any stale semaphore signals
    if (s_player.panel_done_sem) {
        xSemaphoreTake(s_player.panel_done_sem, 0);
    }

    // esp_lcd_panel_draw_bitmap is blocking for DPI panels - it waits for completion
    // So we don't need to wait on semaphore, just call it
    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        s_player.panel_handle,
        0, y,
        VIDEO_W, y + h,
        buf
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(ret));
        s_player.dma_busy[stripe_idx] = false;
        return ret;
    }

    if (s_player.panel_done_sem) {
        if (xSemaphoreTake(s_player.panel_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Panel transfer timeout waiting for stripe %d", stripe_idx);
        }
    }

    // Since draw_bitmap blocks, DMA is complete when it returns
    s_player.dma_busy[stripe_idx] = false;
    xSemaphoreGive(s_player.dma_done_sem[stripe_idx]);

    return ESP_OK;
}

static void playback_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Playback task started");

    // Enter LVGL bypass mode
    if (enter_lvgl_bypass_mode() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter LVGL bypass mode");
        s_player.is_playing = false;
        vTaskDelete(NULL);
        return;
    }
    
    int64_t frame_start_us = esp_timer_get_time();
    s_player.stats_start_time_us = frame_start_us;
    s_player.frame_count_stats = 0;
    
    // Playback loop
    while (!s_player.should_stop) {
        // Handle pause
        if (s_player.is_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        int64_t frame_start_us = esp_timer_get_time();
        int frame_delay_ms = 33;  // Default 30 FPS
        
        if (s_player.format == ANIM_FORMAT_WEBP) {
            // WebP playback
            uint8_t *frame_rgba = NULL;
            int timestamp_ms = 0;
            int ret = WebPAnimDecoderGetNext(s_player.webp_decoder, &frame_rgba, &timestamp_ms);
            
            if (!ret) {
                // End of animation
                if (s_player.should_loop) {
                    WebPAnimDecoderReset(s_player.webp_decoder);
                    s_player.frame_index = 0;
                    ESP_LOGI(TAG, "Looping WebP animation");
                    continue;
                } else {
                    ESP_LOGI(TAG, "WebP animation finished");
                    break;
                }
            }
            
            frame_delay_ms = s_player.frame_delays ? s_player.frame_delays[s_player.frame_index] : 33;
            if (frame_delay_ms < 1) frame_delay_ms = 1;
            
            // Sync frame buffer to cache
            size_t frame_size_rgba = (size_t)s_player.anim_width * s_player.anim_height * 4;
            bool frame_in_spiram = ((uintptr_t)frame_rgba >= 0x40000000 && (uintptr_t)frame_rgba < 0x50000000);
            if (frame_in_spiram) {
                uintptr_t aligned_addr = (uintptr_t)frame_rgba & ~(uintptr_t)0x3F;
                size_t aligned_size = ((frame_size_rgba + ((uintptr_t)frame_rgba - aligned_addr) + 0x3F) / 0x40) * 0x40;
                esp_cache_msync((void *)aligned_addr, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            }
            
            // Decode and send stripes (ping-pong)
            int use_stripe_a = 1;
            for (int y = 0; y < VIDEO_H; y += s_player.stripe_height) {
                int h = (VIDEO_H - y < s_player.stripe_height) ? (VIDEO_H - y) : s_player.stripe_height;
                uint8_t *stripe_buf = use_stripe_a ? s_player.stripeA : s_player.stripeB;
                int stripe_idx = use_stripe_a ? 0 : 1;
                
                convert_rgba_to_rgb888_stripe(
                    frame_rgba,
                    s_player.anim_width,
                    s_player.anim_height,
                    stripe_buf,
                    VIDEO_W,
                    VIDEO_H,
                    y,
                    h
                );
                
                if (send_stripe_dma(stripe_idx, y, h, stripe_buf) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send stripe DMA");
                    break;
                }
                
                use_stripe_a = !use_stripe_a;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            
            s_player.frame_index++;
            if (s_player.frame_index >= s_player.frame_count) {
                s_player.frame_index = 0;
            }
            
        } else if (s_player.format == ANIM_FORMAT_GIF) {
            // GIF playback
            if (!s_player.gif_decoder) {
                ESP_LOGE(TAG, "GIF decoder not initialized");
                break;
            }
            
            // Allocate frame buffer if needed
            if (!s_player.gif_frame_buffer) {
                // Use canvas size for frame buffer (may be larger than frame size)
                size_t frame_size = (size_t)s_player.anim_width * s_player.anim_height * 3;  // RGB888
                s_player.gif_frame_buffer = (uint8_t *)heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM);
                if (!s_player.gif_frame_buffer) {
                    ESP_LOGE(TAG, "Failed to allocate GIF frame buffer");
                    break;
                }
                s_player.gif_frame_buffer_size = frame_size;
                memset(s_player.gif_frame_buffer, 0, frame_size);
            }
            
            // Set draw context for GIF decoder to accumulate full frame
            gif_draw_context_t draw_ctx = {
                .decoder_state = s_player.gif_decoder,
                .stripe_buffer = NULL,  // Don't write to stripe during decode
                .stripe_y = 0,
                .stripe_height = 0,
                .display_width = VIDEO_W,
                .display_height = VIDEO_H,
                .frame_buffer = s_player.gif_frame_buffer,
                .frame_width = s_player.anim_width,   // Canvas width
                .frame_height = s_player.anim_height // Canvas height
            };
            
            gif_decoder_set_draw_context(s_player.gif_decoder, &draw_ctx);
            
            // Decode full frame (GIFDraw callback will accumulate into frame_buffer)
            int delay_ms = 0;
            bool frame_ok = gif_decoder_play_frame(s_player.gif_decoder, &delay_ms);
            
            if (!frame_ok) {
                // End of animation
                if (s_player.should_loop || gif_decoder_get_loop_count(s_player.gif_decoder) == 0) {
                    gif_decoder_reset(s_player.gif_decoder);
                    // Clear frame buffer for fresh start
                    if (s_player.gif_frame_buffer) {
                        memset(s_player.gif_frame_buffer, 0, s_player.gif_frame_buffer_size);
                    }
                    ESP_LOGI(TAG, "Looping GIF animation");
                    continue;
                } else {
                    ESP_LOGI(TAG, "GIF animation finished");
                    break;
                }
            }
            
            frame_delay_ms = delay_ms;
            if (frame_delay_ms < 1) frame_delay_ms = 1;

            s_player.gif_frame_counter++;
            
            // Sync frame buffer to cache if in SPIRAM
            bool frame_in_spiram = ((uintptr_t)s_player.gif_frame_buffer >= 0x40000000 && 
                                   (uintptr_t)s_player.gif_frame_buffer < 0x50000000);
            if (frame_in_spiram) {
                uintptr_t aligned_addr = (uintptr_t)s_player.gif_frame_buffer & ~(uintptr_t)0x3F;
                size_t aligned_size = ((s_player.gif_frame_buffer_size + ((uintptr_t)s_player.gif_frame_buffer - aligned_addr) + 0x3F) / 0x40) * 0x40;
                esp_cache_msync((void *)aligned_addr, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
            
            if (s_player.should_stop) {
                ESP_LOGD(TAG, "[GIF] Stop requested before rendering frame");
                break;
            }

            int use_stripe_a = 1;
            bool stripe_failed = false;
            const uint32_t x_step = (uint32_t)(((uint64_t)s_player.anim_width << 16) / (uint32_t)VIDEO_W);
            const uint32_t y_step = (uint32_t)(((uint64_t)s_player.anim_height << 16) / (uint32_t)VIDEO_H);
            for (int y = 0; y < VIDEO_H && !s_player.should_stop; y += s_player.stripe_height) {
                int h = (VIDEO_H - y < s_player.stripe_height) ? (VIDEO_H - y) : s_player.stripe_height;
                uint8_t *stripe_buf = use_stripe_a ? s_player.stripeA : s_player.stripeB;
                int stripe_idx = use_stripe_a ? 0 : 1;
                uint8_t *stripe_row = stripe_buf;

                if (s_player.anim_width == VIDEO_W && s_player.anim_height == VIDEO_H) {
                    const uint8_t *frame_row = s_player.gif_frame_buffer + (size_t)y * s_player.anim_width * 3;
                    for (int dy = 0; dy < h; ++dy) {
                        const uint8_t *src_row = frame_row + (size_t)dy * s_player.anim_width * 3;
                        uint8_t *dst_row = stripe_row + (size_t)dy * VIDEO_W * 3;
                        memcpy(dst_row, src_row, (size_t)VIDEO_W * 3);
                    }
                } else {
                    uint32_t src_y_acc = (uint32_t)(((uint64_t)y * s_player.anim_height << 16) / (uint32_t)VIDEO_H);
                    for (int dy = 0; dy < h; ++dy) {
                        int src_y = (int)(src_y_acc >> 16);
                        if (src_y >= s_player.anim_height) {
                            src_y = s_player.anim_height - 1;
                        }
                        const uint8_t *src_row = s_player.gif_frame_buffer + (size_t)src_y * s_player.anim_width * 3;
                        uint8_t *dst_row = stripe_row + (size_t)dy * VIDEO_W * 3;
                        uint32_t src_x_acc = 0;
                        for (int dx = 0; dx < VIDEO_W; ++dx) {
                            int src_x = (int)(src_x_acc >> 16);
                            if (src_x >= s_player.anim_width) {
                                src_x = s_player.anim_width - 1;
                            }
                            const uint8_t *src_px = src_row + (size_t)src_x * 3;
                            uint8_t *dst_px = dst_row + (size_t)dx * 3;
                            dst_px[0] = src_px[0];
                            dst_px[1] = src_px[1];
                            dst_px[2] = src_px[2];
                            src_x_acc += x_step;
                        }
                        src_y_acc += y_step;
                    }
                }

                esp_err_t stripe_ret = send_stripe_dma(stripe_idx, y, h, stripe_buf);
                if (stripe_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send GIF stripe DMA (ret=0x%x, y=%d, h=%d)", stripe_ret, y, h);
                    stripe_failed = true;
                    break;
                }

                use_stripe_a = !use_stripe_a;
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            if (stripe_failed) {
                ESP_LOGW(TAG, "[GIF] Stripe transfer aborted for frame %u", (unsigned)s_player.gif_frame_counter);
                break;
            }
        } else {
            ESP_LOGE(TAG, "Unknown animation format");
            break;
        }
        
        int64_t frame_end_us = esp_timer_get_time();
        int64_t frame_time_us = frame_end_us - frame_start_us;
        
        // Update statistics
        s_player.frame_count_stats++;
        if (s_player.frame_count_stats > 0) {
            int64_t elapsed_us = frame_end_us - s_player.stats_start_time_us;
            s_player.current_fps = (s_player.frame_count_stats * 1000000.0f) / elapsed_us;
            s_player.frame_ms_total = frame_time_us / 1000.0f;
        }
        
        // Frame pacing
        int64_t elapsed_ms = frame_time_us / 1000;
        if (elapsed_ms < frame_delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(frame_delay_ms - elapsed_ms));
        }
    }
    
    // Exit LVGL bypass mode unless the caller requested to keep it active
    if (!s_player.keep_bypass_on_stop) {
        exit_lvgl_bypass_mode();
    } else {
        ESP_LOGD(TAG, "Playback task ending with LVGL bypass mode intact");
    }

    // Reset bypass preference for next session
    s_player.keep_bypass_on_stop = false;
    
    // Cleanup decoders
    cleanup_webp_decoder();
    cleanup_gif_decoder();
    
    // Free precomputed scaling index maps
    free_index_maps();
    
    s_player.is_playing = false;
    s_player.is_paused = false;
    s_player.should_stop = false;
    s_player.format = ANIM_FORMAT_UNKNOWN;
    s_player.anim_width = 0;
    s_player.anim_height = 0;
    s_player.keep_bypass_on_stop = false;
    
    ESP_LOGI(TAG, "Playback task ended");
    vTaskDelete(NULL);
}

esp_err_t video_player_play_webp(const uint8_t* file_data, size_t file_size, bool loop)
{
    ESP_LOGI(TAG, "[VIDEO_PLAYER] video_player_play_webp() called - file_size=%zu, loop=%d, is_playing=%d", 
             file_size, loop, s_player.is_playing);
    
    if (s_player.is_playing) {
        ESP_LOGW(TAG, "[VIDEO_PLAYER] Video already playing, stopping first (keeping bypass mode)");
        video_player_stop(true);  // Keep bypass mode active for seamless switch
        vTaskDelay(pdMS_TO_TICKS(50));  // Brief wait for stop to complete
        ESP_LOGI(TAG, "[VIDEO_PLAYER] After stop: is_playing=%d", s_player.is_playing);
    }
    
    if (!s_player.stripeA || !s_player.stripeB) {
        ESP_LOGE(TAG, "[VIDEO_PLAYER] Video player not initialized - stripeA=%p, stripeB=%p", 
                 s_player.stripeA, s_player.stripeB);
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "[VIDEO_PLAYER] Starting WebP playback: size=%zu bytes, loop=%d", file_size, loop);
    
    // Parse WebP animation
    WebPData webp_data = {file_data, file_size};
    WebPAnimDecoderOptions dec_options;
    WebPAnimDecoderOptionsInit(&dec_options);
    dec_options.color_mode = MODE_RGBA;
    
    WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webp_data, &dec_options);
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to create WebP decoder");
        return ESP_ERR_INVALID_ARG;
    }
    
    WebPAnimInfo anim_info;
    if (!WebPAnimDecoderGetInfo(decoder, &anim_info)) {
        ESP_LOGE(TAG, "Failed to get WebP animation info");
        WebPAnimDecoderDelete(decoder);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "WebP animation: %ux%u, %zu frames, loop=%d",
             anim_info.canvas_width, anim_info.canvas_height,
             anim_info.frame_count, anim_info.loop_count);
    
    // Check if animation matches display resolution (or can be scaled)
    if (anim_info.canvas_width != VIDEO_W || anim_info.canvas_height != VIDEO_H) {
        ESP_LOGW(TAG, "Animation size %ux%u doesn't match display %ux%u", 
                 anim_info.canvas_width, anim_info.canvas_height, VIDEO_W, VIDEO_H);
        // For now, we'll decode and it will be cropped/scaled by the decoder
        // TODO: Implement proper scaling if needed
    }
    
    // Allocate frame delays array
    int *frame_delays = (int *)malloc(anim_info.frame_count * sizeof(int));
    if (!frame_delays) {
        ESP_LOGE(TAG, "Failed to allocate frame delays");
        WebPAnimDecoderDelete(decoder);
        return ESP_ERR_NO_MEM;
    }
    
    // Store state
    s_player.webp_decoder = decoder;
    s_player.anim_info = anim_info;
    s_player.webp_data = (uint8_t *)malloc(file_size);
    if (!s_player.webp_data) {
        ESP_LOGE(TAG, "Failed to allocate WebP data copy");
        free(frame_delays);
        WebPAnimDecoderDelete(decoder);
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_player.webp_data, file_data, file_size);
    s_player.webp_size = file_size;
    s_player.frame_delays = frame_delays;
    s_player.frame_count = anim_info.frame_count;
    s_player.frame_index = 0;
    s_player.should_loop = loop;
    s_player.should_stop = false;
    s_player.format = ANIM_FORMAT_WEBP;
    s_player.anim_width = anim_info.canvas_width;
    s_player.anim_height = anim_info.canvas_height;
    
    // Precompute scaling index maps for performance optimization
    esp_err_t idx_ret = compute_index_maps(
        (int)s_player.anim_width,
        (int)s_player.anim_height,
        VIDEO_W,
        VIDEO_H);
    if (idx_ret != ESP_OK) {
        ESP_LOGW(TAG, "Scaling setup failed, falling back to baseline computation: %s", esp_err_to_name(idx_ret));
    } else if (s_player.needs_scaling) {
        ESP_LOGI(TAG, "Scaling animation on-the-fly: %ux%u -> %ux%u",
                 (unsigned)s_player.anim_width,
                 (unsigned)s_player.anim_height,
                 VIDEO_W, VIDEO_H);
    }
    
    // Extract frame delays from demuxer (like in renderer)
    const WebPDemuxer *demux = WebPAnimDecoderGetDemuxer(s_player.webp_decoder);
    if (demux) {
        WebPIterator iter;
        if (WebPDemuxGetFrame(demux, 1, &iter)) {
            do {
                size_t idx = (size_t)iter.frame_num - 1;
                if (idx < anim_info.frame_count) {
                    frame_delays[idx] = iter.duration;
                }
            } while (WebPDemuxNextFrame(&iter));
            WebPDemuxReleaseIterator(&iter);
        }
    }
    
    // Set fallback delays
    for (size_t i = 0; i < anim_info.frame_count; ++i) {
        if (frame_delays[i] <= 0) {
            frame_delays[i] = 33;  // Default 30 FPS
        }
    }
    
    // Create playback task
    s_player.is_playing = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        playback_task,
        "video_playback",
        8192,  // Stack size
        NULL,
        5,     // Priority (high)
        &s_player.playback_task,
        1
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "[VIDEO_PLAYER] Failed to create playback task");
        free(frame_delays);
        free(s_player.webp_data);
        WebPAnimDecoderDelete(s_player.webp_decoder);
        s_player.webp_decoder = NULL;
        s_player.is_playing = false;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "[VIDEO_PLAYER] Playback started successfully - is_playing=%d", s_player.is_playing);
    return ESP_OK;
}

anim_format_t video_player_detect_format(const char *file_path)
{
    if (!file_path) {
        return ANIM_FORMAT_UNKNOWN;
    }
    
    anim_format_t format = detect_format_from_path(file_path);
    if (format != ANIM_FORMAT_UNKNOWN) {
        return format;
    }
    
    // Try reading file header
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        return ANIM_FORMAT_UNKNOWN;
    }
    
    uint8_t header[12];
    size_t read = fread(header, 1, sizeof(header), f);
    fclose(f);
    
    if (read == sizeof(header)) {
        format = detect_format_from_header(header, sizeof(header));
    }
    
    return format;
}

esp_err_t video_player_play_file(const char *file_path, bool loop)
{
    if (!file_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    anim_format_t format = video_player_detect_format(file_path);
    
    if (format == ANIM_FORMAT_WEBP) {
        // Read file into memory and call play_webp
        FILE *f = fopen(file_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open file: %s", file_path);
            return ESP_ERR_NOT_FOUND;
        }
        
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
            fclose(f);
            ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
            return ESP_ERR_INVALID_SIZE;
        }
        
        uint8_t *data = (uint8_t *)malloc((size_t)file_size);
        if (!data) {
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
        
        size_t read = fread(data, 1, (size_t)file_size, f);
        fclose(f);
        
        if (read != (size_t)file_size) {
            free(data);
            return ESP_FAIL;
        }
        
        esp_err_t ret = video_player_play_webp(data, (size_t)file_size, loop);
        // Note: data is owned by video_player until cleanup
        return ret;
    } else if (format == ANIM_FORMAT_GIF) {
        return video_player_play_gif(file_path, loop);
    } else {
        ESP_LOGE(TAG, "Unknown or unsupported format: %s", file_path);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t video_player_play_gif(const char *file_path, bool loop)
{
    ESP_LOGI(TAG, "[VIDEO_PLAYER] video_player_play_gif() called - path=%s, loop=%d", 
             file_path, loop);
    
    if (s_player.is_playing) {
        ESP_LOGW(TAG, "[VIDEO_PLAYER] Video already playing, stopping first");
        video_player_stop(true);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    if (!s_player.stripeA || !s_player.stripeB) {
        ESP_LOGE(TAG, "[VIDEO_PLAYER] Video player not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Initialize GIF decoder
    gif_decoder_state_t *gif_decoder = (gif_decoder_state_t *)malloc(sizeof(gif_decoder_state_t));
    if (!gif_decoder) {
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = gif_decoder_init(gif_decoder);
    if (ret != ESP_OK) {
        free(gif_decoder);
        return ret;
    }
    
    ret = gif_decoder_open_file(gif_decoder, file_path);
    if (ret != ESP_OK) {
        gif_decoder_close(gif_decoder);
        free(gif_decoder);
        return ret;
    }
    
    int canvas_w = 0, canvas_h = 0;
    gif_decoder_get_canvas_size(gif_decoder, &canvas_w, &canvas_h);
    
    // Store state
    s_player.gif_decoder = gif_decoder;
    s_player.gif_file_path = strdup(file_path);
    s_player.should_loop = loop;
    s_player.should_stop = false;
    s_player.is_paused = false;
    s_player.format = ANIM_FORMAT_GIF;
    s_player.anim_width = canvas_w;
    s_player.anim_height = canvas_h;
    s_player.gif_frame_counter = 0;
    
    gif_decoder_set_loop(gif_decoder, loop);
    
    ESP_LOGI(TAG, "GIF animation: %dx%d, loop=%d", canvas_w, canvas_h, loop);
    
    // Create playback task
    s_player.is_playing = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        playback_task,
        "video_playback",
        8192,
        NULL,
        5,
        &s_player.playback_task,
        1
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "[VIDEO_PLAYER] Failed to create playback task");
        cleanup_gif_decoder();
        s_player.is_playing = false;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "[VIDEO_PLAYER] GIF playback started successfully");
    return ESP_OK;
}

esp_err_t video_player_pause(void)
{
    if (!s_player.is_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.is_paused = true;
    ESP_LOGI(TAG, "Playback paused");
    return ESP_OK;
}

esp_err_t video_player_resume(void)
{
    if (!s_player.is_playing || !s_player.is_paused) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.is_paused = false;
    ESP_LOGI(TAG, "Playback resumed");
    return ESP_OK;
}

esp_err_t video_player_stop(bool keep_bypass)
{
    ESP_LOGI(TAG, "[VIDEO_PLAYER] video_player_stop() called - is_playing=%d, keep_bypass=%d", 
             s_player.is_playing, keep_bypass);
    
    if (!s_player.is_playing) {
        ESP_LOGI(TAG, "[VIDEO_PLAYER] Already stopped, returning");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "[VIDEO_PLAYER] Stopping video playback...");
    
    s_player.keep_bypass_on_stop = keep_bypass;
    ESP_LOGI(TAG, "[VIDEO_PLAYER] keep_bypass_on_stop set to %d", s_player.keep_bypass_on_stop);
    s_player.should_stop = true;
    
    // Wait for playback task to finish
    int timeout_ms = 5000;
    while (s_player.is_playing && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_ms -= 100;
    }
    
    if (s_player.is_playing) {
        ESP_LOGW(TAG, "[VIDEO_PLAYER] Playback task didn't stop gracefully, deleting task");
        if (s_player.playback_task) {
            vTaskDelete(s_player.playback_task);
            s_player.playback_task = NULL;
        }
        if (!keep_bypass) {
            exit_lvgl_bypass_mode();
        }
        s_player.is_playing = false;
    } else {
        // Only exit bypass mode if not keeping it active
        if (!keep_bypass) {
            exit_lvgl_bypass_mode();
        }
    }
    
    ESP_LOGI(TAG, "[VIDEO_PLAYER] Video playback stopped - is_playing=%d", s_player.is_playing);
    return ESP_OK;
}

bool video_player_is_playing(void)
{
    return s_player.is_playing;
}

esp_err_t video_player_get_stats(float *fps_out, float *decode_ms_out, 
                                 float *dma_ms_out, float *frame_ms_out)
{
    if (fps_out) *fps_out = s_player.current_fps;
    if (decode_ms_out) *decode_ms_out = s_player.decode_ms_per_stripe;
    if (dma_ms_out) *dma_ms_out = s_player.dma_ms_per_stripe;
    if (frame_ms_out) *frame_ms_out = s_player.frame_ms_total;
    return ESP_OK;
}

