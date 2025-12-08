#include "pico8_render.h"
#include "display_renderer.h"
#include "pico8_logo_data.h"
#include "p3a_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "pico8_render";

// RGB color type
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pico8_color_t;

// Default PICO-8 palette
static const pico8_color_t s_pico8_palette_defaults[PICO8_PALETTE_COLORS] = {
    {0x00, 0x00, 0x00}, {0x1D, 0x2B, 0x53}, {0x7E, 0x25, 0x53}, {0x00, 0x87, 0x51},
    {0xAB, 0x52, 0x36}, {0x5F, 0x57, 0x4F}, {0xC2, 0xC3, 0xC7}, {0xFF, 0xF1, 0xE8},
    {0xFF, 0x00, 0x4D}, {0xFF, 0xA3, 0x00}, {0xFF, 0xEC, 0x27}, {0x00, 0xE4, 0x36},
    {0x29, 0xAD, 0xFF}, {0x83, 0x76, 0x9C}, {0xFF, 0x77, 0xA8}, {0xFF, 0xCC, 0xAA},
};

// Internal state
static struct {
    uint8_t *frame_buffers[2];      // Double-buffered RGBA frames
    uint8_t decode_index;           // Buffer being written to
    uint8_t display_index;          // Buffer being displayed
    bool frame_ready;               // A new frame is ready to display
    int64_t last_frame_time_us;     // Timestamp of last frame
    uint16_t *lookup_x;             // X coordinate lookup table
    uint16_t *lookup_y;             // Y coordinate lookup table
    pico8_color_t palette[PICO8_PALETTE_COLORS];  // Current palette
    bool palette_initialized;
    SemaphoreHandle_t mutex;
    bool initialized;
} s_pico8 = {0};

// ============================================================================
// Pixel storage helpers
// ============================================================================

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
static inline uint16_t logo_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}
#endif

static inline void store_pixel(uint8_t *buffer, size_t row_stride, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!buffer || x < 0 || x >= P3A_DISPLAY_WIDTH || y < 0 || y >= P3A_DISPLAY_HEIGHT) {
        return;
    }
    
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
    uint16_t *row = (uint16_t *)(buffer + (size_t)y * row_stride);
    const size_t row_pixels = row_stride / sizeof(uint16_t);
    if ((size_t)x >= row_pixels) {
        return;
    }
    row[x] = logo_rgb565(r, g, b);
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
    uint8_t *row = buffer + (size_t)y * row_stride;
    const size_t idx = (size_t)x * 3U;
    if ((idx + 2) >= row_stride) {
        return;
    }
    row[idx + 0] = b;
    row[idx + 1] = g;
    row[idx + 2] = r;
#endif
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t pico8_render_init(void)
{
    if (s_pico8.initialized) {
        return ESP_OK;
    }
    
    const size_t frame_bytes = (size_t)PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT * 4;
    
    // Allocate frame buffers
    for (int i = 0; i < 2; ++i) {
        if (!s_pico8.frame_buffers[i]) {
            s_pico8.frame_buffers[i] = (uint8_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_pico8.frame_buffers[i]) {
                ESP_LOGE(TAG, "Failed to allocate PICO-8 frame buffer %d", i);
                pico8_render_deinit();
                return ESP_ERR_NO_MEM;
            }
            memset(s_pico8.frame_buffers[i], 0, frame_bytes);
        }
    }
    
    // Allocate lookup tables
    if (!s_pico8.lookup_x) {
        s_pico8.lookup_x = (uint16_t *)heap_caps_malloc(P3A_DISPLAY_WIDTH * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!s_pico8.lookup_x) {
            ESP_LOGE(TAG, "Failed to allocate PICO-8 lookup X table");
            pico8_render_deinit();
            return ESP_ERR_NO_MEM;
        }
        for (int dst_x = 0; dst_x < P3A_DISPLAY_WIDTH; ++dst_x) {
            int src_x = (dst_x * PICO8_FRAME_WIDTH) / P3A_DISPLAY_WIDTH;
            if (src_x >= PICO8_FRAME_WIDTH) {
                src_x = PICO8_FRAME_WIDTH - 1;
            }
            s_pico8.lookup_x[dst_x] = (uint16_t)src_x;
        }
    }
    
    if (!s_pico8.lookup_y) {
        s_pico8.lookup_y = (uint16_t *)heap_caps_malloc(P3A_DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!s_pico8.lookup_y) {
            ESP_LOGE(TAG, "Failed to allocate PICO-8 lookup Y table");
            pico8_render_deinit();
            return ESP_ERR_NO_MEM;
        }
        for (int dst_y = 0; dst_y < P3A_DISPLAY_HEIGHT; ++dst_y) {
            int src_y = (dst_y * PICO8_FRAME_HEIGHT) / P3A_DISPLAY_HEIGHT;
            if (src_y >= PICO8_FRAME_HEIGHT) {
                src_y = PICO8_FRAME_HEIGHT - 1;
            }
            s_pico8.lookup_y[dst_y] = (uint16_t)src_y;
        }
    }
    
    // Initialize palette
    if (!s_pico8.palette_initialized) {
        memcpy(s_pico8.palette, s_pico8_palette_defaults, sizeof(s_pico8.palette));
        s_pico8.palette_initialized = true;
    }
    
    // Create mutex
    if (!s_pico8.mutex) {
        s_pico8.mutex = xSemaphoreCreateMutex();
        if (!s_pico8.mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            pico8_render_deinit();
            return ESP_ERR_NO_MEM;
        }
    }
    
    s_pico8.initialized = true;
    ESP_LOGI(TAG, "PICO-8 render initialized");
    
    return ESP_OK;
}

void pico8_render_deinit(void)
{
    for (int i = 0; i < 2; ++i) {
        if (s_pico8.frame_buffers[i]) {
            free(s_pico8.frame_buffers[i]);
            s_pico8.frame_buffers[i] = NULL;
        }
    }
    
    if (s_pico8.lookup_x) {
        heap_caps_free(s_pico8.lookup_x);
        s_pico8.lookup_x = NULL;
    }
    
    if (s_pico8.lookup_y) {
        heap_caps_free(s_pico8.lookup_y);
        s_pico8.lookup_y = NULL;
    }
    
    if (s_pico8.mutex) {
        vSemaphoreDelete(s_pico8.mutex);
        s_pico8.mutex = NULL;
    }
    
    s_pico8.frame_ready = false;
    s_pico8.last_frame_time_us = 0;
    s_pico8.palette_initialized = false;
    s_pico8.initialized = false;
    
    ESP_LOGI(TAG, "PICO-8 render deinitialized");
}

bool pico8_render_is_initialized(void)
{
    return s_pico8.initialized;
}

// ============================================================================
// Frame submission
// ============================================================================

esp_err_t pico8_render_submit_frame(const uint8_t *palette_rgb, size_t palette_len,
                                    const uint8_t *pixel_data, size_t pixel_len)
{
    if (!pixel_data || pixel_len < PICO8_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize if needed
    esp_err_t err = pico8_render_init();
    if (err != ESP_OK) {
        return err;
    }
    
    // Update palette if provided
    if (palette_rgb && palette_len >= (PICO8_PALETTE_COLORS * 3)) {
        for (int i = 0; i < PICO8_PALETTE_COLORS; ++i) {
            size_t idx = (size_t)i * 3;
            s_pico8.palette[i].r = palette_rgb[idx + 0];
            s_pico8.palette[i].g = palette_rgb[idx + 1];
            s_pico8.palette[i].b = palette_rgb[idx + 2];
        }
    }
    
    // Get decode buffer
    const uint8_t target_index = s_pico8.decode_index & 0x01;
    uint8_t *target = s_pico8.frame_buffers[target_index];
    if (!target) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Decode packed 4bpp pixels to RGBA
    const size_t total_pixels = (size_t)PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT;
    size_t pixel_cursor = 0;
    
    for (size_t i = 0; i < PICO8_FRAME_BYTES && i < pixel_len; ++i) {
        uint8_t packed = pixel_data[i];
        uint8_t low_idx = packed & 0x0F;
        uint8_t high_idx = (packed >> 4) & 0x0F;
        
        pico8_color_t low_color = s_pico8.palette[low_idx];
        pico8_color_t high_color = s_pico8.palette[high_idx];
        
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
    
    // Swap buffers (with mutex protection)
    const int64_t now = esp_timer_get_time();
    
    if (s_pico8.mutex && xSemaphoreTake(s_pico8.mutex, portMAX_DELAY) == pdTRUE) {
        s_pico8.display_index = target_index;
        s_pico8.decode_index = target_index ^ 1;
        s_pico8.frame_ready = true;
        s_pico8.last_frame_time_us = now;
        xSemaphoreGive(s_pico8.mutex);
    } else {
        s_pico8.display_index = target_index;
        s_pico8.decode_index = target_index ^ 1;
        s_pico8.frame_ready = true;
        s_pico8.last_frame_time_us = now;
    }
    
    return ESP_OK;
}

// ============================================================================
// Frame rendering
// ============================================================================

int pico8_render_logo(uint8_t *dest_buffer, size_t row_stride)
{
    if (!dest_buffer) {
        return -1;
    }
    
    // Clear to black
    size_t total_bytes = row_stride * P3A_DISPLAY_HEIGHT;
    memset(dest_buffer, 0, total_bytes);
    
    // Logo dimensions (6x upscale)
    const int logo_src_w = PICO8_LOGO_WIDTH;
    const int logo_src_h = PICO8_LOGO_HEIGHT;
    const int logo_dst_w = logo_src_w * 6;
    const int logo_dst_h = logo_src_h * 6;
    
    // Center position
    const int logo_x = (P3A_DISPLAY_WIDTH - logo_dst_w) / 2;
    const int logo_y = (P3A_DISPLAY_HEIGHT - logo_dst_h) / 2;
    
    // Render logo with 6x nearest neighbor upscale
    for (int dst_y = 0; dst_y < logo_dst_h; dst_y++) {
        int src_y = dst_y / 6;
        if (src_y >= logo_src_h) {
            src_y = logo_src_h - 1;
        }
        
        for (int dst_x = 0; dst_x < logo_dst_w; dst_x++) {
            int src_x = dst_x / 6;
            if (src_x >= logo_src_w) {
                src_x = logo_src_w - 1;
            }
            
            // Calculate source pixel index
            int src_idx = (src_y * logo_src_w + src_x) * 4;
            if (src_idx >= PICO8_LOGO_SIZE) {
                continue;
            }
            
            uint8_t r = pico8_logo_data[src_idx + 0];
            uint8_t g = pico8_logo_data[src_idx + 1];
            uint8_t b = pico8_logo_data[src_idx + 2];
            
            int final_x = logo_x + dst_x;
            int final_y = logo_y + dst_y;
            
            store_pixel(dest_buffer, row_stride, final_x, final_y, r, g, b);
        }
    }
    
    return 16; // Frame delay
}

int pico8_render_frame(uint8_t *dest_buffer, size_t row_stride)
{
    if (!dest_buffer) {
        return -1;
    }
    
    // If not initialized or no frame ready, show logo
    if (!s_pico8.initialized || !s_pico8.frame_ready) {
        return pico8_render_logo(dest_buffer, row_stride);
    }
    
    // Get source buffer
    uint8_t *src = NULL;
    if (s_pico8.mutex && xSemaphoreTake(s_pico8.mutex, portMAX_DELAY) == pdTRUE) {
        src = s_pico8.frame_buffers[s_pico8.display_index];
        xSemaphoreGive(s_pico8.mutex);
    } else {
        src = s_pico8.frame_buffers[s_pico8.display_index];
    }
    
    if (!src) {
        return pico8_render_logo(dest_buffer, row_stride);
    }
    
    // Use display_renderer for parallel upscaling
    // Note: PICO-8 doesn't use rotation (always ROTATION_0)
    display_renderer_parallel_upscale(src, PICO8_FRAME_WIDTH, PICO8_FRAME_HEIGHT,
                                      dest_buffer,
                                      s_pico8.lookup_x, s_pico8.lookup_y,
                                      DISPLAY_ROTATION_0);
    
    return 16; // Frame delay (~60fps)
}

bool pico8_render_frame_ready(void)
{
    bool ready = false;
    
    if (s_pico8.mutex && xSemaphoreTake(s_pico8.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ready = s_pico8.frame_ready;
        xSemaphoreGive(s_pico8.mutex);
    } else {
        ready = s_pico8.frame_ready;
    }
    
    return ready;
}

void pico8_render_mark_consumed(void)
{
    // Currently we don't clear frame_ready since we want to keep showing
    // the last frame until a new one arrives or timeout
}

int64_t pico8_render_get_last_frame_time(void)
{
    int64_t time = 0;
    
    if (s_pico8.mutex && xSemaphoreTake(s_pico8.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        time = s_pico8.last_frame_time_us;
        xSemaphoreGive(s_pico8.mutex);
    } else {
        time = s_pico8.last_frame_time_us;
    }
    
    return time;
}

