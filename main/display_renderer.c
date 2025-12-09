#include "display_renderer_priv.h"
#include "ugfx_ui.h"
#include "config_store.h"

#ifdef CONFIG_P3A_USE_PIE_SIMD
#include "pie_memcpy_128.h"
#endif

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
uint8_t *g_upscale_dst_buffer = NULL;
const uint16_t *g_upscale_lookup_x = NULL;
const uint16_t *g_upscale_lookup_y = NULL;
int g_upscale_src_w = 0;
int g_upscale_src_h = 0;
display_rotation_t g_upscale_rotation = DISPLAY_ROTATION_0;
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
    wait_for_render_mode(DISPLAY_RENDER_MODE_UI);
    ESP_LOGI(DISPLAY_TAG, "UI mode active");
    return ESP_OK;
}

void display_renderer_exit_ui_mode(void)
{
    ESP_LOGI(DISPLAY_TAG, "Exiting UI mode");
    g_display_mode_request = DISPLAY_RENDER_MODE_ANIMATION;
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
    
    config_store_set_rotation(rotation);
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

void display_renderer_blit_upscaled(const uint8_t *src_rgba, int src_w, int src_h,
                                    uint8_t *dst_buffer,
                                    const uint16_t *lookup_x, const uint16_t *lookup_y,
                                    display_rotation_t rotation)
{
    if (!src_rgba || !dst_buffer || src_w <= 0 || src_h <= 0) {
        return;
    }

    if (!lookup_x || !lookup_y) {
        ESP_LOGE(DISPLAY_TAG, "Upscale lookup tables not initialized");
        return;
    }

    const int dst_w = EXAMPLE_LCD_H_RES;
    const int dst_h = EXAMPLE_LCD_V_RES;
    const uint32_t *src_rgba32 = (const uint32_t *)src_rgba;

    for (int dst_y = 0; dst_y < dst_h; ++dst_y) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)dst_y * g_display_row_stride);
#else
        uint8_t *dst_row = dst_buffer + (size_t)dst_y * g_display_row_stride;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[dst_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
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
                const uint16_t src_x_fixed = lookup_x[dst_y];
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_y = lookup_y[dst_x];
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
                const uint16_t raw_src_y = lookup_y[dst_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_x = lookup_x[dst_x];
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
                const uint16_t raw_src_x = lookup_x[dst_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_y = lookup_y[dst_x];
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
                const uint16_t src_y = lookup_y[dst_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
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

// Row-range version for worker tasks
// Like the OLD code, reads g_display_row_stride directly (set once during init, never changes)
static void blit_upscaled_rows(const uint8_t *src_rgba, int src_w, int src_h,
                               uint8_t *dst_buffer, int dst_w, int dst_h,
                               int row_start, int row_end,
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

#ifdef CONFIG_P3A_USE_PIE_SIMD
    // PIE SIMD optimization: Detect vertically duplicated rows and use pie_memcpy_128
    // to copy them instead of re-rendering. Works for all rotation modes.
    
    int dst_y = row_start;
    while (dst_y < row_end) {
        // Find how many consecutive destination rows produce identical output
        int run_start = dst_y;
        dst_y++;
        
        // Check if consecutive rows are identical based on rotation mode
        int rows_match = 1;
        while (dst_y < row_end && rows_match) {
            switch (rotation) {
                case DISPLAY_ROTATION_0:
                case DISPLAY_ROTATION_180:
                    rows_match = (lookup_y[run_start] == lookup_y[dst_y]);
                    break;
                case DISPLAY_ROTATION_90:
                case DISPLAY_ROTATION_270:
                    rows_match = (lookup_x[run_start] == lookup_x[dst_y]);
                    break;
                default:
                    rows_match = (lookup_y[run_start] == lookup_y[dst_y]);
                    break;
            }
            if (rows_match) {
                dst_y++;
            }
        }
        int run_end = dst_y; // First row AFTER this run
        
        // Render the first row of this vertical run using scalar code
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)run_start * g_display_row_stride);
#else
        uint8_t *dst_row = dst_buffer + (size_t)run_start * g_display_row_stride;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[run_start];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
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
                const uint16_t src_x_fixed = lookup_x[run_start];
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_y = lookup_y[dst_x];
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
                const uint16_t raw_src_y = lookup_y[run_start];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_x = lookup_x[dst_x];
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
                const uint16_t raw_src_x = lookup_x[run_start];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_y = lookup_y[dst_x];
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
                const uint16_t src_y = lookup_y[run_start];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
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
        
        // Copy the rendered row to all subsequent rows in this vertical run using PIE SIMD
        uint8_t *src_row_ptr = dst_buffer + (size_t)run_start * g_display_row_stride;
        for (int ddy = run_start + 1; ddy < run_end; ++ddy) {
            uint8_t *dst_row_ptr = dst_buffer + (size_t)ddy * g_display_row_stride;
            pie_memcpy_128(dst_row_ptr, src_row_ptr, g_display_row_stride);
        }
    }
    return;
#endif

    // Standard scalar path for all rotations (or when PIE SIMD is disabled)
    // Read g_display_row_stride directly in loop like OLD code does with s_frame_row_stride_bytes
    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)dst_y * g_display_row_stride);
#else
        uint8_t *dst_row = dst_buffer + (size_t)dst_y * g_display_row_stride;
#endif

        switch (rotation) {
            case DISPLAY_ROTATION_0: {
                const uint16_t src_y = lookup_y[dst_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
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
                const uint16_t src_x_fixed = lookup_x[dst_y];
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_y = lookup_y[dst_x];
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
                const uint16_t raw_src_y = lookup_y[dst_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_x = lookup_x[dst_x];
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
                const uint16_t raw_src_x = lookup_x[dst_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_y = lookup_y[dst_x];
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
                const uint16_t src_y = lookup_y[dst_y];
                const uint32_t *src_row32 = src_rgba32 + (size_t)src_y * src_w;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
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
            blit_upscaled_rows(g_upscale_src_buffer,
                               g_upscale_src_w, g_upscale_src_h,
                               g_upscale_dst_buffer,
                               EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                               g_upscale_row_start_top, g_upscale_row_end_top,
                               g_upscale_lookup_x, g_upscale_lookup_y,
                               g_upscale_rotation);
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
            blit_upscaled_rows(g_upscale_src_buffer,
                               g_upscale_src_w, g_upscale_src_h,
                               g_upscale_dst_buffer,
                               EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                               g_upscale_row_start_bottom, g_upscale_row_end_bottom,
                               g_upscale_lookup_x, g_upscale_lookup_y,
                               g_upscale_rotation);
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
                                       display_rotation_t rotation)
{
    const int dst_h = EXAMPLE_LCD_V_RES;

    // DIAGNOSTIC: Use only top worker to process ALL rows (glitch-free configuration)
    // Set to 0 to enable two-worker parallel mode (for testing cache coherency fixes)
    #define DISPLAY_UPSCALE_SINGLE_WORKER 0

#if !DISPLAY_UPSCALE_SINGLE_WORKER
    const int mid_row = dst_h / 2;
#endif

    // Set up shared state exactly like OLD code
    g_upscale_src_buffer = src_rgba;
    g_upscale_dst_buffer = dst_buffer;
    g_upscale_lookup_x = lookup_x;
    g_upscale_lookup_y = lookup_y;
    g_upscale_src_w = src_w;
    g_upscale_src_h = src_h;
    g_upscale_rotation = rotation;
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
        // If not using the post-draw wait, block for VSYNC before rendering
#if !CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
        if (use_vsync) {
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
        }
#endif

        // Check render mode
        const display_render_mode_t mode = g_display_mode_request;
        g_display_mode_active = mode;
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
                if (residual_us > 1000) {
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
#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
        if (use_vsync) {
            xSemaphoreTake(g_display_vsync_sem, 0); // clear any stale
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
        }
#endif

        // Update timing stats after the frame has been presented
        g_last_frame_present_us = esp_timer_get_time();

        // Yield if not using vsync
        if (!use_vsync) {
            vTaskDelay(1);
        }
    }
}

