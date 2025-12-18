#include "display_renderer_priv.h"
#include "ugfx_ui.h"
#include "config_store.h"
#include <string.h>  // for memcpy

// LCD panel state
esp_lcd_panel_handle_t g_display_panel = NULL;
uint8_t **g_display_buffers = NULL;
uint8_t g_display_buffer_count = 0;
size_t g_display_buffer_bytes = 0;
size_t g_display_row_stride = 0;

// Synchronization
SemaphoreHandle_t g_display_vsync_sem = NULL;
SemaphoreHandle_t g_display_mutex = NULL;
TaskHandle_t g_display_render_task = NULL;

// Render mode
volatile display_render_mode_t g_display_mode_request = DISPLAY_RENDER_MODE_ANIMATION;
volatile display_render_mode_t g_display_mode_active = DISPLAY_RENDER_MODE_ANIMATION;

// Frame callback
display_frame_callback_t g_display_frame_callback = NULL;
void *g_display_frame_callback_ctx = NULL;

// Upscale worker tasks
TaskHandle_t g_upscale_worker_top = NULL;
TaskHandle_t g_upscale_worker_bottom = NULL;
TaskHandle_t g_upscale_main_task = NULL;

// Upscale shared state (set per-frame before notifying workers)
const uint8_t *g_upscale_src_buffer = NULL;
int g_upscale_src_bpp = 4; // 4 for RGBA8888, 3 for RGB888
uint8_t *g_upscale_dst_buffer = NULL;
const uint16_t *g_upscale_lookup_x = NULL;
const uint16_t *g_upscale_lookup_y = NULL;
int g_upscale_src_w = 0;
int g_upscale_src_h = 0;
display_rotation_t g_upscale_rotation = DISPLAY_ROTATION_0;
int g_upscale_offset_x = 0;
int g_upscale_offset_y = 0;
int g_upscale_scaled_w = 0;
int g_upscale_scaled_h = 0;
volatile bool g_upscale_has_borders = false;
uint8_t g_upscale_bg_r = 0;
uint8_t g_upscale_bg_g = 0;
uint8_t g_upscale_bg_b = 0;
uint16_t g_upscale_bg_rgb565 = 0;
int g_upscale_row_start_top = 0;
int g_upscale_row_end_top = 0;
int g_upscale_row_start_bottom = 0;
int g_upscale_row_end_bottom = 0;
volatile bool g_upscale_worker_top_done = false;
volatile bool g_upscale_worker_bottom_done = false;

// Buffer management
uint8_t g_render_buffer_index = 0;
uint8_t g_last_display_buffer = 0;

// Timing
int64_t g_last_frame_present_us = 0;
uint32_t g_target_frame_delay_ms = 0;

// Screen rotation
display_rotation_t g_screen_rotation = DISPLAY_ROTATION_0;
volatile bool g_rotation_in_progress = false;

// Forward declarations
static esp_err_t prepare_vsync(void);
static void wait_for_render_mode(display_render_mode_t target_mode);

// ============================================================================
// RGB conversion helpers
// ============================================================================

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

// ============================================================================
// FPS overlay
// ============================================================================

// FPS tracking state
static int64_t s_fps_last_time_us = 0;
static uint32_t s_fps_frame_count = 0;
static uint32_t s_fps_current = 0;
static bool s_fps_show_cached = true;
static int64_t s_fps_config_check_time = 0;

// Simple 5x7 bitmap font for digits 0-9 and space
// Each digit is 5 pixels wide, 7 pixels tall, stored as 7 bytes (1 byte per row)
static const uint8_t s_font_5x7[11][7] = {
    // '0'
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    // '1'
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    // '2'
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    // '3'
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    // '4'
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    // '5'
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    // '6'
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    // '7'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    // '8'
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    // '9'
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    // ' ' (space)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

// Draw a single pixel at (x, y) with color (r, g, b)
static inline void fps_draw_pixel(uint8_t *buffer, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= EXAMPLE_LCD_H_RES || y < 0 || y >= EXAMPLE_LCD_V_RES) return;
    
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
    uint16_t *row = (uint16_t *)(buffer + (size_t)y * g_display_row_stride);
    row[x] = rgb565(r, g, b);
#else
    uint8_t *row = buffer + (size_t)y * g_display_row_stride;
    size_t idx = (size_t)x * 3U;
    row[idx + 0] = b;
    row[idx + 1] = g;
    row[idx + 2] = r;
#endif
}

// Draw a character at position (x, y) with given colors, scale factor
static void fps_draw_char(uint8_t *buffer, int x, int y, int ch_idx, int scale,
                          uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                          uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    if (ch_idx < 0 || ch_idx > 10) return;
    
    for (int row = 0; row < 7; row++) {
        uint8_t bits = s_font_5x7[ch_idx][row];
        for (int col = 0; col < 5; col++) {
            bool pixel_on = (bits >> (4 - col)) & 1;
            uint8_t r = pixel_on ? fg_r : bg_r;
            uint8_t g = pixel_on ? fg_g : bg_g;
            uint8_t b = pixel_on ? fg_b : bg_b;
            
            // Draw scaled pixel
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fps_draw_pixel(buffer, x + col * scale + sx, y + row * scale + sy, r, g, b);
                }
            }
        }
    }
}

// Draw FPS overlay on top-right of screen
static void fps_draw_overlay(uint8_t *buffer, uint32_t fps)
{
    // Format: "XXX" (up to 3 digits)
    char fps_str[8];
    int len = snprintf(fps_str, sizeof(fps_str), "%lu", (unsigned long)fps);
    if (len < 0 || len >= (int)sizeof(fps_str)) len = 3;
    
    const int scale = 2;  // 2x scale for better visibility
    const int char_w = 5 * scale + scale;  // Character width + spacing
    const int char_h = 7 * scale;
    const int padding = 6;
    
    // Position at top-right
    int total_width = len * char_w - scale;  // Remove trailing space
    int x = EXAMPLE_LCD_H_RES - total_width - padding;
    int y = padding;
    
    // Draw background rectangle (semi-transparent effect via darker bg)
    int bg_x = x - 4;
    int bg_y = y - 2;
    int bg_w = total_width + 8;
    int bg_h = char_h + 4;
    
    for (int by = bg_y; by < bg_y + bg_h && by < EXAMPLE_LCD_V_RES; by++) {
        for (int bx = bg_x; bx < bg_x + bg_w && bx < EXAMPLE_LCD_H_RES; bx++) {
            if (bx >= 0 && by >= 0) {
                fps_draw_pixel(buffer, bx, by, 0, 0, 0);
            }
        }
    }
    
    // Draw each digit
    for (int i = 0; i < len; i++) {
        int ch_idx = 10;  // space by default
        if (fps_str[i] >= '0' && fps_str[i] <= '9') {
            ch_idx = fps_str[i] - '0';
        }
        fps_draw_char(buffer, x + i * char_w, y, ch_idx, scale,
                      255, 255, 255,  // White foreground
                      0, 0, 0);       // Black background
    }
}

// Update FPS counter and optionally draw overlay
static void fps_update_and_draw(uint8_t *buffer)
{
    int64_t now_us = esp_timer_get_time();
    
    // Check config every second to avoid frequent NVS reads
    if (now_us - s_fps_config_check_time > 1000000) {
        s_fps_show_cached = config_store_get_show_fps();
        s_fps_config_check_time = now_us;
    }
    
    // Always track FPS (for debugging/logging)
    s_fps_frame_count++;
    
    if (s_fps_last_time_us == 0) {
        s_fps_last_time_us = now_us;
        return;
    }
    
    int64_t elapsed_us = now_us - s_fps_last_time_us;
    if (elapsed_us >= 1000000) {
        // Calculate FPS
        s_fps_current = (uint32_t)((uint64_t)s_fps_frame_count * 1000000ULL / (uint64_t)elapsed_us);
        s_fps_frame_count = 0;
        s_fps_last_time_us = now_us;
    }
    
    // Draw overlay if enabled
    if (s_fps_show_cached && buffer && s_fps_current > 0) {
        fps_draw_overlay(buffer, s_fps_current);
    }
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t display_renderer_init(esp_lcd_panel_handle_t panel,
                                uint8_t **buffers,
                                uint8_t buffer_count,
                                size_t buffer_bytes,
                                size_t row_stride)
{
    if (!panel || !buffers || buffer_count == 0 || buffer_bytes == 0 || row_stride == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    g_display_panel = panel;
    g_display_buffers = buffers;
    g_display_buffer_count = buffer_count;
    g_display_buffer_bytes = buffer_bytes;
    g_display_row_stride = row_stride;

    esp_err_t err = prepare_vsync();
    if (err != ESP_OK) {
        return err;
    }

    g_display_mutex = xSemaphoreCreateMutex();
    if (!g_display_mutex) {
        ESP_LOGE(DISPLAY_TAG, "Failed to create display mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create upscale worker tasks
    if (g_upscale_worker_top == NULL) {
        if (xTaskCreatePinnedToCore(display_upscale_worker_top_task,
                                    "upscale_top",
                                    2048,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &g_upscale_worker_top,
                                    0) != pdPASS) {
            ESP_LOGE(DISPLAY_TAG, "Failed to create top upscale worker task");
            vSemaphoreDelete(g_display_mutex);
            g_display_mutex = NULL;
            return ESP_FAIL;
        }
    }

    if (g_upscale_worker_bottom == NULL) {
        if (xTaskCreatePinnedToCore(display_upscale_worker_bottom_task,
                                    "upscale_bottom",
                                    2048,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &g_upscale_worker_bottom,
                                    1) != pdPASS) {
            ESP_LOGE(DISPLAY_TAG, "Failed to create bottom upscale worker task");
            if (g_upscale_worker_top) {
                vTaskDelete(g_upscale_worker_top);
                g_upscale_worker_top = NULL;
            }
            vSemaphoreDelete(g_display_mutex);
            g_display_mutex = NULL;
            return ESP_FAIL;
        }
    }

    // Load saved rotation
    display_rotation_t saved_rotation = (display_rotation_t)config_store_get_rotation();
    if (saved_rotation != DISPLAY_ROTATION_0) {
        ESP_LOGI(DISPLAY_TAG, "Restoring saved rotation: %d degrees", saved_rotation);
        g_screen_rotation = saved_rotation;
        ugfx_ui_set_rotation(saved_rotation);
    }

    ESP_LOGI(DISPLAY_TAG, "Display renderer initialized");
    return ESP_OK;
}

void display_renderer_deinit(void)
{
    if (g_display_render_task) {
        vTaskDelete(g_display_render_task);
        g_display_render_task = NULL;
    }

    if (g_upscale_worker_top) {
        vTaskDelete(g_upscale_worker_top);
        g_upscale_worker_top = NULL;
    }

    if (g_upscale_worker_bottom) {
        vTaskDelete(g_upscale_worker_bottom);
        g_upscale_worker_bottom = NULL;
    }

    if (g_display_mutex) {
        vSemaphoreDelete(g_display_mutex);
        g_display_mutex = NULL;
    }

    // Unregister the ISR callback BEFORE deleting the semaphore to avoid
    // the ISR trying to give a deleted semaphore (causes watchdog timeout)
    if (g_display_panel && g_display_vsync_sem) {
        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_refresh_done = NULL,
        };
        esp_lcd_dpi_panel_register_event_callbacks(g_display_panel, &cbs, NULL);
    }

    if (g_display_vsync_sem) {
        vSemaphoreDelete(g_display_vsync_sem);
        g_display_vsync_sem = NULL;
    }

    g_display_panel = NULL;
    g_display_buffers = NULL;
    g_display_buffer_count = 0;
}

esp_err_t display_renderer_start(void)
{
    if (g_display_render_task == NULL) {
        if (xTaskCreate(display_render_task,
                        "display_render",
                        4096,
                        NULL,
                        CONFIG_P3A_RENDER_TASK_PRIORITY,
                        &g_display_render_task) != pdPASS) {
            ESP_LOGE(DISPLAY_TAG, "Failed to start display render task");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

// ============================================================================
// Frame callback
// ============================================================================

void display_renderer_set_frame_callback(display_frame_callback_t callback, void *user_ctx)
{
    if (g_display_mutex && xSemaphoreTake(g_display_mutex, portMAX_DELAY) == pdTRUE) {
        g_display_frame_callback = callback;
        g_display_frame_callback_ctx = user_ctx;
        xSemaphoreGive(g_display_mutex);
    } else {
        g_display_frame_callback = callback;
        g_display_frame_callback_ctx = user_ctx;
    }
}

// ============================================================================
// Mode switching
// ============================================================================

static void wait_for_render_mode(display_render_mode_t target_mode)
{
    const TickType_t check_delay = pdMS_TO_TICKS(5);
    const TickType_t timeout = pdMS_TO_TICKS(500);
    TickType_t waited = 0;

    while (g_display_mode_active != target_mode) {
        vTaskDelay(check_delay);
        waited += check_delay;
        if (waited >= timeout) {
            ESP_LOGW(DISPLAY_TAG, "Timed out waiting for render mode %d (active=%d)",
                     target_mode, g_display_mode_active);
            break;
        }
    }
}

esp_err_t display_renderer_enter_ui_mode(void)
{
    ESP_LOGI(DISPLAY_TAG, "Entering UI mode");
    g_display_mode_request = DISPLAY_RENDER_MODE_UI;
    
    // Give VSYNC semaphore multiple times to wake up render task
    // The task may be blocked at either of two VSYNC wait points:
    // 1. Before rendering (CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW=0)
    // 2. After rendering (CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW=1)
    // Giving multiple times ensures we wake it from either point
    if (g_display_vsync_sem) {
        xSemaphoreGive(g_display_vsync_sem);
        // Short delay then give again in case task is at second wait point
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreGive(g_display_vsync_sem);
    }
    
    wait_for_render_mode(DISPLAY_RENDER_MODE_UI);
    ESP_LOGI(DISPLAY_TAG, "UI mode active");
    return ESP_OK;
}

void display_renderer_exit_ui_mode(void)
{
    ESP_LOGI(DISPLAY_TAG, "Exiting UI mode");
    g_display_mode_request = DISPLAY_RENDER_MODE_ANIMATION;
    
    // In UI mode the task yields with vTaskDelay, so it will naturally
    // pick up the mode change. Give semaphore just in case.
    if (g_display_vsync_sem) {
        xSemaphoreGive(g_display_vsync_sem);
    }
    
    wait_for_render_mode(DISPLAY_RENDER_MODE_ANIMATION);
    ESP_LOGI(DISPLAY_TAG, "Animation mode active");
}

bool display_renderer_is_ui_mode(void)
{
    return (g_display_mode_active == DISPLAY_RENDER_MODE_UI);
}

// ============================================================================
// Rotation
// ============================================================================

esp_err_t display_renderer_set_rotation(display_rotation_t rotation)
{
    if (rotation != DISPLAY_ROTATION_0 && rotation != DISPLAY_ROTATION_90 && 
        rotation != DISPLAY_ROTATION_180 && rotation != DISPLAY_ROTATION_270) {
        ESP_LOGE(DISPLAY_TAG, "Invalid rotation angle: %d", rotation);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_rotation_in_progress) {
        ESP_LOGW(DISPLAY_TAG, "Rotation operation already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (rotation == g_screen_rotation) {
        ESP_LOGI(DISPLAY_TAG, "Already at rotation %d degrees", rotation);
        return ESP_OK;
    }
    
    g_rotation_in_progress = true;
    display_rotation_t old_rotation = g_screen_rotation;
    g_screen_rotation = rotation;
    
    ESP_LOGI(DISPLAY_TAG, "Setting screen rotation from %d to %d degrees", old_rotation, rotation);
    
    esp_err_t err = ugfx_ui_set_rotation(rotation);
    if (err != ESP_OK) {
        ESP_LOGW(DISPLAY_TAG, "Failed to set µGFX rotation: %s", esp_err_to_name(err));
    }
    
    config_store_set_rotation((uint16_t)rotation);
    g_rotation_in_progress = false;
    
    ESP_LOGI(DISPLAY_TAG, "Screen rotation set to %d degrees", rotation);
    return ESP_OK;
}

display_rotation_t display_renderer_get_rotation(void)
{
    return g_screen_rotation;
}

// ============================================================================
// Dimension queries
// ============================================================================

void display_renderer_get_dimensions(int *width, int *height, size_t *stride)
{
    if (width) *width = EXAMPLE_LCD_H_RES;
    if (height) *height = EXAMPLE_LCD_V_RES;
    if (stride) *stride = g_display_row_stride;
}

size_t display_renderer_get_buffer_bytes(void)
{
    return g_display_buffer_bytes;
}

// ============================================================================
// Vsync setup
// ============================================================================

static esp_err_t prepare_vsync(void)
{
    if (g_display_buffer_count > 1) {
        if (g_display_vsync_sem == NULL) {
            g_display_vsync_sem = xSemaphoreCreateBinary();
        }
        if (g_display_vsync_sem == NULL) {
            ESP_LOGE(DISPLAY_TAG, "Failed to allocate VSYNC semaphore");
            return ESP_ERR_NO_MEM;
        }
        (void)xSemaphoreTake(g_display_vsync_sem, 0);
        xSemaphoreGive(g_display_vsync_sem);

        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_refresh_done = display_panel_refresh_done_cb,
        };
        return esp_lcd_dpi_panel_register_event_callbacks(g_display_panel, &cbs, g_display_vsync_sem);
    }

    if (g_display_vsync_sem) {
        vSemaphoreDelete(g_display_vsync_sem);
        g_display_vsync_sem = NULL;
        ESP_LOGW(DISPLAY_TAG, "Single LCD frame buffer in use; tearing may occur");
    }
    return ESP_OK;
}

bool display_panel_refresh_done_cb(esp_lcd_panel_handle_t panel, 
                                   esp_lcd_dpi_panel_event_data_t *edata, 
                                   void *user_ctx)
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

// ============================================================================
// Upscale/blit implementation
// ============================================================================

// Diagnostic: Use only top worker to process ALL rows (glitch-free configuration)
// Set to 0 to enable two-worker parallel mode (for testing cache coherency fixes)
#define DISPLAY_UPSCALE_SINGLE_WORKER 0

// Row-range version for worker tasks
// Like the OLD code, reads g_display_row_stride directly (set once during init, never changes)
static void blit_upscaled_rows_rgba(const uint8_t *src_rgba, int src_w, int src_h,
                                    uint8_t *dst_buffer, int dst_w, int dst_h,
                                    int row_start, int row_end,
                                    int offset_x, int offset_y, int scaled_w, int scaled_h,
                                    const uint16_t *lookup_x, const uint16_t *lookup_y,
                                    display_rotation_t rotation)
{
    if (!src_rgba || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    if (!lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale lookup tables not initialized");
        return;
    }

    const uint32_t *src_rgba32 = (const uint32_t *)src_rgba;

#if CONFIG_P3A_USE_PIE_SIMD
    // Row duplication optimization: detect runs of consecutive dst rows that map to the same source.
    // Instead of re-rendering duplicate rows, we copy from the first rendered row of each run.
    // For ROTATION_0/180: use lookup_y to detect vertical runs
    // For ROTATION_90/270: use lookup_x to detect vertical runs (axes are swapped)
    const uint16_t *run_lookup = (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270)
                                  ? lookup_x : lookup_y;
    uint16_t prev_run_key = UINT16_MAX;  // impossible initial value
    uint8_t *last_rendered_row = NULL;
#endif

    // Read g_display_row_stride directly in loop like OLD code does with s_frame_row_stride_bytes
    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
        if (scaled_w <= 0 || scaled_h <= 0) {
            continue;
        }
        if (dst_y < offset_y || dst_y >= (offset_y + scaled_h)) {
            continue; // outside image region; borders filled in second pass
        }
        const int local_y = dst_y - offset_y;

        uint8_t *dst_row_bytes = dst_buffer + (size_t)dst_y * g_display_row_stride;

#if CONFIG_P3A_USE_PIE_SIMD
        // Check if this row is a duplicate of the previous rendered row
        const uint16_t run_key = run_lookup[local_y];
        if (run_key == prev_run_key && last_rendered_row != NULL) {
            // This row is identical to the last rendered row - copy it using memcpy
            // Standard memcpy is cache-coherent and well-optimized
            memcpy(dst_row_bytes, last_rendered_row, g_display_row_stride);
            continue;
        }
        // New run: render this row normally and remember it
        prev_run_key = run_key;
        last_rendered_row = dst_row_bytes;
#endif

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)dst_row_bytes;
#else
        uint8_t *dst_row = dst_row_bytes;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[local_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint32_t rgba = src_row32[src_x];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            case DISPLAY_ROTATION_90: {
                const uint16_t src_x_fixed = lookup_x[local_y];
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_y = lookup_y[local_x];
                    const uint16_t src_y = (src_h - 1) - raw_src_y;
                    const uint32_t rgba = src_rgba32[(size_t)src_y * src_w + src_x_fixed];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            case DISPLAY_ROTATION_180: {
                const uint16_t raw_src_y = lookup_y[local_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_x = lookup_x[local_x];
                    const uint16_t src_x = (src_w - 1) - raw_src_x;
                    const uint32_t rgba = src_row32[src_x];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            case DISPLAY_ROTATION_270: {
                const uint16_t raw_src_x = lookup_x[local_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_y = lookup_y[local_x];
                    const uint32_t rgba = src_rgba32[(size_t)src_y * src_w + src_x_fixed];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
            
            default: {
                const uint16_t src_y = lookup_y[local_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint32_t rgba = src_row32[src_x];
                    const uint8_t r = rgba & 0xFF;
                    const uint8_t g = (rgba >> 8) & 0xFF;
                    const uint8_t b = (rgba >> 16) & 0xFF;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
        }
    }
}

static void blit_upscaled_rows_rgb(const uint8_t *src_rgb, int src_w, int src_h,
                                   uint8_t *dst_buffer, int dst_w, int dst_h,
                                   int row_start, int row_end,
                                   int offset_x, int offset_y, int scaled_w, int scaled_h,
                                   const uint16_t *lookup_x, const uint16_t *lookup_y,
                                   display_rotation_t rotation)
{
    if (!src_rgb || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    if (!lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale lookup tables not initialized");
        return;
    }

#if CONFIG_P3A_USE_PIE_SIMD
    // Row duplication optimization: detect runs of consecutive dst rows that map to the same source.
    // Instead of re-rendering duplicate rows, we copy from the first rendered row of each run.
    // For ROTATION_0/180: use lookup_y to detect vertical runs
    // For ROTATION_90/270: use lookup_x to detect vertical runs (axes are swapped)
    const uint16_t *run_lookup = (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270)
                                  ? lookup_x : lookup_y;
    uint16_t prev_run_key = UINT16_MAX;  // impossible initial value
    uint8_t *last_rendered_row = NULL;
#endif

    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
        if (scaled_w <= 0 || scaled_h <= 0) {
            continue;
        }
        if (dst_y < offset_y || dst_y >= (offset_y + scaled_h)) {
            continue;
        }
        const int local_y = dst_y - offset_y;

        uint8_t *dst_row_bytes = dst_buffer + (size_t)dst_y * g_display_row_stride;

#if CONFIG_P3A_USE_PIE_SIMD
        // Check if this row is a duplicate of the previous rendered row
        const uint16_t run_key = run_lookup[local_y];
        if (run_key == prev_run_key && last_rendered_row != NULL) {
            // This row is identical to the last rendered row - copy it using memcpy
            // Standard memcpy is cache-coherent and well-optimized
            memcpy(dst_row_bytes, last_rendered_row, g_display_row_stride);
            continue;
        }
        // New run: render this row normally and remember it
        prev_run_key = run_key;
        last_rendered_row = dst_row_bytes;
#endif

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)dst_row_bytes;
#else
        uint8_t *dst_row = dst_row_bytes;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[local_y];
                const uint8_t *src_row = src_rgb + (size_t)src_y * (size_t)src_w * 3U;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint8_t *p = src_row + (size_t)src_x * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            case DISPLAY_ROTATION_90: {
                const uint16_t src_x_fixed = lookup_x[local_y];
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_y = lookup_y[local_x];
                    const uint16_t src_y = (src_h - 1) - raw_src_y;
                    const uint8_t *p = src_rgb + ((size_t)src_y * (size_t)src_w + (size_t)src_x_fixed) * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            case DISPLAY_ROTATION_180: {
                const uint16_t raw_src_y = lookup_y[local_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint8_t *src_row = src_rgb + (size_t)src_y * (size_t)src_w * 3U;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t raw_src_x = lookup_x[local_x];
                    const uint16_t src_x = (src_w - 1) - raw_src_x;
                    const uint8_t *p = src_row + (size_t)src_x * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            case DISPLAY_ROTATION_270: {
                const uint16_t raw_src_x = lookup_x[local_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_y = lookup_y[local_x];
                    const uint8_t *p = src_rgb + ((size_t)src_y * (size_t)src_w + (size_t)src_x_fixed) * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }

            default: {
                const uint16_t src_y = lookup_y[local_y];
                const uint8_t *src_row = src_rgb + (size_t)src_y * (size_t)src_w * 3U;
                for (int dst_x = offset_x; dst_x < (offset_x + scaled_w); ++dst_x) {
                    const int local_x = dst_x - offset_x;
                    const uint16_t src_x = lookup_x[local_x];
                    const uint8_t *p = src_row + (size_t)src_x * 3U;
                    const uint8_t r = p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = p[2];
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(r, g, b);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    dst_row[idx + 0] = b;
                    dst_row[idx + 1] = g;
                    dst_row[idx + 2] = r;
#endif
                }
                break;
            }
        }
    }
}

// ============================================================================
// Border fill (second pass after upscale)
// ============================================================================

#if CONFIG_LCD_PIXEL_FORMAT_RGB888
static inline void fill_rgb888_pixels(uint8_t *row, int x_start, int x_end, uint8_t b, uint8_t g, uint8_t r)
{
    if (!row || x_start >= x_end) return;
    uint8_t *p = row + (size_t)x_start * 3U;
    int count = x_end - x_start;

    // Unroll 4 pixels at a time (12 bytes)
    while (count >= 4) {
        p[0] = b; p[1] = g; p[2] = r;
        p[3] = b; p[4] = g; p[5] = r;
        p[6] = b; p[7] = g; p[8] = r;
        p[9] = b; p[10] = g; p[11] = r;
        p += 12;
        count -= 4;
    }
    while (count-- > 0) {
        p[0] = b; p[1] = g; p[2] = r;
        p += 3;
    }
}
#endif

static void fill_borders_rows(uint8_t *dst_buffer, int dst_w, int dst_h,
                              int row_start, int row_end,
                              int offset_x, int offset_y, int scaled_w, int scaled_h)
{
    if (!dst_buffer || dst_w <= 0 || dst_h <= 0) return;

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    const int img_x0 = offset_x;
    const int img_x1 = offset_x + scaled_w;
    const int img_y0 = offset_y;
    const int img_y1 = offset_y + scaled_h;

    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)dst_y * g_display_row_stride);
#else
        uint8_t *dst_row = dst_buffer + (size_t)dst_y * g_display_row_stride;
#endif

        if (dst_y < img_y0 || dst_y >= img_y1) {
            // Entire row is border
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
            const uint16_t bg = g_upscale_bg_rgb565;
            uintptr_t addr = (uintptr_t)dst_row;
            int x = 0;
            if ((addr & 0x3U) && dst_w > 0) {
                dst_row[0] = bg;
                x = 1;
            }
            const uint32_t bg32 = ((uint32_t)bg << 16) | (uint32_t)bg;
            uint32_t *p32 = (uint32_t *)(dst_row + x);
            const int pairs = (dst_w - x) / 2;
            for (int i = 0; i < pairs; ++i) {
                p32[i] = bg32;
            }
            const int filled = x + pairs * 2;
            if (filled < dst_w) {
                dst_row[filled] = bg;
            }
#else
            fill_rgb888_pixels(dst_row, 0, dst_w, g_upscale_bg_b, g_upscale_bg_g, g_upscale_bg_r);
#endif
        } else {
            // Left border
            if (img_x0 > 0) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                const uint16_t bg = g_upscale_bg_rgb565;
                for (int x = 0; x < img_x0; ++x) {
                    dst_row[x] = bg;
                }
#else
                fill_rgb888_pixels(dst_row, 0, img_x0, g_upscale_bg_b, g_upscale_bg_g, g_upscale_bg_r);
#endif
            }
            // Right border
            if (img_x1 < dst_w) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                const uint16_t bg = g_upscale_bg_rgb565;
                for (int x = img_x1; x < dst_w; ++x) {
                    dst_row[x] = bg;
                }
#else
                fill_rgb888_pixels(dst_row, img_x1, dst_w, g_upscale_bg_b, g_upscale_bg_g, g_upscale_bg_r);
#endif
            }
        }
    }
}

// ============================================================================
// Upscale workers
// ============================================================================

void display_upscale_worker_top_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 0);

    while (true) {
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);

        DISPLAY_MEMORY_BARRIER();

        if (g_upscale_src_buffer && g_upscale_dst_buffer &&
            g_upscale_row_start_top < g_upscale_row_end_top) {
            if (g_upscale_src_bpp == 3) {
                blit_upscaled_rows_rgb(g_upscale_src_buffer,
                                       g_upscale_src_w, g_upscale_src_h,
                                       g_upscale_dst_buffer,
                                       EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                       g_upscale_row_start_top, g_upscale_row_end_top,
                                       g_upscale_offset_x, g_upscale_offset_y,
                                       g_upscale_scaled_w, g_upscale_scaled_h,
                                       g_upscale_lookup_x, g_upscale_lookup_y,
                                       g_upscale_rotation);
            } else {
                blit_upscaled_rows_rgba(g_upscale_src_buffer,
                                        g_upscale_src_w, g_upscale_src_h,
                                        g_upscale_dst_buffer,
                                        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                        g_upscale_row_start_top, g_upscale_row_end_top,
                                        g_upscale_offset_x, g_upscale_offset_y,
                                        g_upscale_scaled_w, g_upscale_scaled_h,
                                        g_upscale_lookup_x, g_upscale_lookup_y,
                                        g_upscale_rotation);
            }
            if (g_upscale_has_borders) {
                fill_borders_rows(g_upscale_dst_buffer,
                                  EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                  g_upscale_row_start_top, g_upscale_row_end_top,
                                  g_upscale_offset_x, g_upscale_offset_y,
                                  g_upscale_scaled_w, g_upscale_scaled_h);
            }
            // NOTE: No per-worker cache flush - OLD code doesn't have it
            // The main task does a full buffer flush after both workers complete
        }

        DISPLAY_MEMORY_BARRIER();

        g_upscale_worker_top_done = true;
        if (g_upscale_main_task) {
            xTaskNotify(g_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

void display_upscale_worker_bottom_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 1);

    while (true) {
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);

        DISPLAY_MEMORY_BARRIER();

        if (g_upscale_src_buffer && g_upscale_dst_buffer &&
            g_upscale_row_start_bottom < g_upscale_row_end_bottom) {
            if (g_upscale_src_bpp == 3) {
                blit_upscaled_rows_rgb(g_upscale_src_buffer,
                                       g_upscale_src_w, g_upscale_src_h,
                                       g_upscale_dst_buffer,
                                       EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                       g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                                       g_upscale_offset_x, g_upscale_offset_y,
                                       g_upscale_scaled_w, g_upscale_scaled_h,
                                       g_upscale_lookup_x, g_upscale_lookup_y,
                                       g_upscale_rotation);
            } else {
                blit_upscaled_rows_rgba(g_upscale_src_buffer,
                                        g_upscale_src_w, g_upscale_src_h,
                                        g_upscale_dst_buffer,
                                        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                        g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                                        g_upscale_offset_x, g_upscale_offset_y,
                                        g_upscale_scaled_w, g_upscale_scaled_h,
                                        g_upscale_lookup_x, g_upscale_lookup_y,
                                        g_upscale_rotation);
            }
            if (g_upscale_has_borders) {
                fill_borders_rows(g_upscale_dst_buffer,
                                  EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                  g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                                  g_upscale_offset_x, g_upscale_offset_y,
                                  g_upscale_scaled_w, g_upscale_scaled_h);
            }
            // NOTE: No per-worker cache flush - OLD code doesn't have it
            // The main task does a full buffer flush after both workers complete
        }

        DISPLAY_MEMORY_BARRIER();

        g_upscale_worker_bottom_done = true;
        if (g_upscale_main_task) {
            xTaskNotify(g_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

void display_renderer_parallel_upscale(const uint8_t *src_rgba, int src_w, int src_h,
                                       uint8_t *dst_buffer,
                                       const uint16_t *lookup_x, const uint16_t *lookup_y,
                                       int offset_x, int offset_y, int scaled_w, int scaled_h,
                                       bool has_borders,
                                       display_rotation_t rotation)
{
    const int dst_h = EXAMPLE_LCD_V_RES;
    const int dst_w = EXAMPLE_LCD_H_RES;

    // Validate parameters to prevent heap corruption from out-of-bounds access
    if (!src_rgba || !dst_buffer || !lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale: NULL pointer (src=%p dst=%p lx=%p ly=%p)",
                 src_rgba, dst_buffer, lookup_x, lookup_y);
        return;
    }
    if (src_w <= 0 || src_h <= 0 || scaled_w <= 0 || scaled_h <= 0) {
        ESP_LOGE(DISPLAY_TAG, "Upscale: Invalid dimensions (src %dx%d, scaled %dx%d)",
                 src_w, src_h, scaled_w, scaled_h);
        return;
    }
    if (offset_x < 0 || offset_y < 0 || offset_x + scaled_w > dst_w || offset_y + scaled_h > dst_h) {
        ESP_LOGE(DISPLAY_TAG, "Upscale: Offset/scaled out of bounds (offset %d,%d scaled %dx%d dst %dx%d)",
                 offset_x, offset_y, scaled_w, scaled_h, dst_w, dst_h);
        return;
    }

#if !DISPLAY_UPSCALE_SINGLE_WORKER
    const int mid_row = dst_h / 2;
#endif

    // Set up shared state exactly like OLD code
    g_upscale_src_buffer = src_rgba;
    g_upscale_src_bpp = 4;
    g_upscale_dst_buffer = dst_buffer;
    g_upscale_lookup_x = lookup_x;
    g_upscale_lookup_y = lookup_y;
    g_upscale_src_w = src_w;
    g_upscale_src_h = src_h;
    g_upscale_rotation = rotation;
    g_upscale_offset_x = offset_x;
    g_upscale_offset_y = offset_y;
    g_upscale_scaled_w = scaled_w;
    g_upscale_scaled_h = scaled_h;
    g_upscale_has_borders = has_borders;
    config_store_get_background_color(&g_upscale_bg_r, &g_upscale_bg_g, &g_upscale_bg_b);
    g_upscale_bg_rgb565 = rgb565(g_upscale_bg_r, g_upscale_bg_g, g_upscale_bg_b);
    g_upscale_main_task = xTaskGetCurrentTaskHandle();

    g_upscale_worker_top_done = false;
    g_upscale_worker_bottom_done = false;

#if DISPLAY_UPSCALE_SINGLE_WORKER
    // Single worker mode: top worker handles ALL rows, bottom worker does nothing
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = dst_h;
    g_upscale_row_start_bottom = dst_h;
    g_upscale_row_end_bottom = dst_h;
#else
    // Split exactly like OLD code: top [0, mid_row), bottom [mid_row, dst_h)
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = mid_row;
    g_upscale_row_start_bottom = mid_row;
    g_upscale_row_end_bottom = dst_h;
#endif

    // Single memory barrier before notifications (matches OLD code exactly)
    DISPLAY_MEMORY_BARRIER();

    // Notify both workers back-to-back (matches OLD code exactly - no barrier between)
    if (g_upscale_worker_top && g_upscale_worker_bottom) {
        xTaskNotify(g_upscale_worker_top, 1, eSetBits);
        xTaskNotify(g_upscale_worker_bottom, 1, eSetBits);
    }

    // Wait for both workers using notification bits
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

    if (!g_upscale_worker_top_done || !g_upscale_worker_bottom_done) {
        ESP_LOGW(DISPLAY_TAG, "Upscale workers may not have completed properly");
    }

    DISPLAY_MEMORY_BARRIER();
}

void display_renderer_parallel_upscale_rgb(const uint8_t *src_rgb, int src_w, int src_h,
                                          uint8_t *dst_buffer,
                                          const uint16_t *lookup_x, const uint16_t *lookup_y,
                                          int offset_x, int offset_y, int scaled_w, int scaled_h,
                                          bool has_borders,
                                          display_rotation_t rotation)
{
    const int dst_h = EXAMPLE_LCD_V_RES;
    const int dst_w = EXAMPLE_LCD_H_RES;

    // Validate parameters to prevent heap corruption from out-of-bounds access
    if (!src_rgb || !dst_buffer || !lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale RGB: NULL pointer (src=%p dst=%p lx=%p ly=%p)",
                 src_rgb, dst_buffer, lookup_x, lookup_y);
        return;
    }
    if (src_w <= 0 || src_h <= 0 || scaled_w <= 0 || scaled_h <= 0) {
        ESP_LOGE(DISPLAY_TAG, "Upscale RGB: Invalid dimensions (src %dx%d, scaled %dx%d)",
                 src_w, src_h, scaled_w, scaled_h);
        return;
    }
    if (offset_x < 0 || offset_y < 0 || offset_x + scaled_w > dst_w || offset_y + scaled_h > dst_h) {
        ESP_LOGE(DISPLAY_TAG, "Upscale RGB: Offset/scaled out of bounds (offset %d,%d scaled %dx%d dst %dx%d)",
                 offset_x, offset_y, scaled_w, scaled_h, dst_w, dst_h);
        return;
    }

#if !DISPLAY_UPSCALE_SINGLE_WORKER
    const int mid_row = dst_h / 2;
#endif

    g_upscale_src_buffer = src_rgb;
    g_upscale_src_bpp = 3;
    g_upscale_dst_buffer = dst_buffer;
    g_upscale_lookup_x = lookup_x;
    g_upscale_lookup_y = lookup_y;
    g_upscale_src_w = src_w;
    g_upscale_src_h = src_h;
    g_upscale_rotation = rotation;
    g_upscale_offset_x = offset_x;
    g_upscale_offset_y = offset_y;
    g_upscale_scaled_w = scaled_w;
    g_upscale_scaled_h = scaled_h;
    g_upscale_has_borders = has_borders;
    config_store_get_background_color(&g_upscale_bg_r, &g_upscale_bg_g, &g_upscale_bg_b);
    g_upscale_bg_rgb565 = rgb565(g_upscale_bg_r, g_upscale_bg_g, g_upscale_bg_b);
    g_upscale_main_task = xTaskGetCurrentTaskHandle();

    g_upscale_worker_top_done = false;
    g_upscale_worker_bottom_done = false;

#if DISPLAY_UPSCALE_SINGLE_WORKER
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = dst_h;
    g_upscale_row_start_bottom = dst_h;
    g_upscale_row_end_bottom = dst_h;
#else
    g_upscale_row_start_top = 0;
    g_upscale_row_end_top = mid_row;
    g_upscale_row_start_bottom = mid_row;
    g_upscale_row_end_bottom = dst_h;
#endif

    DISPLAY_MEMORY_BARRIER();

    if (g_upscale_worker_top && g_upscale_worker_bottom) {
        xTaskNotify(g_upscale_worker_top, 1, eSetBits);
        xTaskNotify(g_upscale_worker_bottom, 1, eSetBits);
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

    if (!g_upscale_worker_top_done || !g_upscale_worker_bottom_done) {
        ESP_LOGW(DISPLAY_TAG, "Upscale workers may not have completed properly");
    }

    DISPLAY_MEMORY_BARRIER();
}

// ============================================================================
// Main render task
// ============================================================================

void display_render_task(void *arg)
{
    (void)arg;

    const bool use_vsync = (g_display_buffer_count > 1) && (g_display_vsync_sem != NULL);
    const uint8_t buffer_count = (g_display_buffer_count == 0) ? 1 : g_display_buffer_count;
    int64_t frame_processing_start_us = 0;

    // Drain any pending VSYNC so the first wait does not return immediately
#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
    if (use_vsync) {
        xSemaphoreTake(g_display_vsync_sem, 0);
    }
#endif

    while (true) {
        // Check render mode - must update g_display_mode_active for wait_for_render_mode()
        display_render_mode_t mode = g_display_mode_request;
        g_display_mode_active = mode;
        
        // If not using the post-draw wait, block for VSYNC before rendering
        // Skip VSYNC wait in UI mode - we use timer-based 10 FPS instead
        // Check g_display_mode_request directly in case it changed while we were blocked
#if !CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
        if (use_vsync && g_display_mode_request != DISPLAY_RENDER_MODE_UI) {
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
            // Re-check mode after waking - it may have changed while we were blocked
            mode = g_display_mode_request;
            g_display_mode_active = mode;
        }
#endif

        const bool ui_mode = (mode == DISPLAY_RENDER_MODE_UI);

        // Get the back buffer
        uint8_t back_buffer_idx = g_render_buffer_index;
        uint8_t *back_buffer = g_display_buffers[back_buffer_idx];
        
        if (!back_buffer) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        frame_processing_start_us = esp_timer_get_time();
        
        int frame_delay_ms = 100;
        uint32_t prev_frame_delay_ms = g_target_frame_delay_ms;

        // =====================================================================
        // UI MODE: Render UI directly to back buffer
        // =====================================================================
        if (ui_mode) {
            frame_delay_ms = ugfx_ui_render_to_buffer(back_buffer, g_display_row_stride);
            if (frame_delay_ms < 0) {
                memset(back_buffer, 0, g_display_buffer_bytes);
                frame_delay_ms = 100;
            }
            g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
        }
        // =====================================================================
        // ANIMATION MODE: Call frame callback
        // =====================================================================
        else {
            display_frame_callback_t callback = g_display_frame_callback;
            void *ctx = g_display_frame_callback_ctx;
            
            if (callback) {
                prev_frame_delay_ms = g_target_frame_delay_ms;
                frame_delay_ms = callback(back_buffer, ctx);
                if (frame_delay_ms < 0) {
                    // No frame available, reuse last buffer
                    back_buffer_idx = g_last_display_buffer;
                    if (back_buffer_idx >= buffer_count) back_buffer_idx = 0;
                    back_buffer = g_display_buffers[back_buffer_idx];
                    frame_delay_ms = 100;
                }
                g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
            } else {
                // No callback set, show black
                memset(back_buffer, 0, g_display_buffer_bytes);
                frame_delay_ms = 100;
                g_target_frame_delay_ms = 100;
            }
        }

        // =====================================================================
        // FPS overlay (drawn after content, before cache sync)
        // =====================================================================
        fps_update_and_draw(back_buffer);

        // =====================================================================
        // Cache sync
        // =====================================================================
#if DISPLAY_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
        esp_cache_msync(back_buffer, g_display_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif

        // =====================================================================
        // Flip buffers
        // =====================================================================
        g_last_display_buffer = back_buffer_idx;
        g_render_buffer_index = (back_buffer_idx + 1) % buffer_count;

        // =====================================================================
        // Wait for frame timing
        // =====================================================================
        if (!APP_LCD_MAX_SPEED_PLAYBACK_ENABLED) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t processing_time_us = now_us - frame_processing_start_us;
            const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;

            if (processing_time_us < target_delay_us) {
                const int64_t residual_us = target_delay_us - processing_time_us;
                if (residual_us > 2000) {
                    vTaskDelay(pdMS_TO_TICKS((residual_us + 500) / 1000));
                }
            }
        }

        // =====================================================================
        // DMA present
        // =====================================================================
        if (app_lcd_get_brightness() == 0) {
            memset(back_buffer, 0, g_display_buffer_bytes);
#if DISPLAY_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
            esp_cache_msync(back_buffer, g_display_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        }

        esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);

        // Wait for frame transfer to complete (tear-free, double-buffer safe)
        // Skip VSYNC wait in UI mode - check g_display_mode_request directly
        // in case mode changed mid-iteration
#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
        if (use_vsync && g_display_mode_request != DISPLAY_RENDER_MODE_UI) {
            xSemaphoreTake(g_display_vsync_sem, 0); // clear any stale
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
        }
#endif

        // Update timing stats after the frame has been presented
        g_last_frame_present_us = esp_timer_get_time();

        // In UI mode, yield to let other tasks run (we're not VSYNC-synced)
        // Also yield if not using vsync - check current mode request
        if (!use_vsync || g_display_mode_request == DISPLAY_RENDER_MODE_UI) {
            vTaskDelay(1);
        }
    }
}

