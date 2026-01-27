// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file display_renderer.c
 * @brief Display renderer core - initialization, mode switching, rotation, render task
 */

#include "display_renderer_priv.h"
#include "ugfx_ui.h"
#include "config_store.h"
#include <string.h>

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
int g_upscale_src_bpp = 4;
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

// Processing notification state
volatile proc_notif_state_t g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
volatile int64_t g_proc_notif_start_time_us = 0;
volatile int64_t g_proc_notif_fail_time_us = 0;

// Forward declarations
static esp_err_t prepare_vsync(void);
static void wait_for_render_mode(display_render_mode_t target_mode);
static esp_err_t create_upscale_workers(void);

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

esp_err_t display_renderer_ensure_upscale_workers(void)
{
    if (g_upscale_worker_top && g_upscale_worker_bottom) {
        return ESP_OK;
    }

    return create_upscale_workers();
}

static esp_err_t create_upscale_workers(void)
{
    if (!g_upscale_worker_top) {
        if (xTaskCreatePinnedToCore(display_upscale_worker_top_task,
                                    "upscale_top",
                                    2048,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &g_upscale_worker_top,
                                    0) != pdPASS) {
            ESP_LOGE(DISPLAY_TAG, "Failed to create top upscale worker task");
            return ESP_FAIL;
        }
    }

    if (!g_upscale_worker_bottom) {
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
            return ESP_FAIL;
        }
    }

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
        // Pin to core 1 for cache locality with bottom upscale worker
        if (xTaskCreatePinnedToCore(display_render_task,
                                    "display_render",
                                    4096,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &g_display_render_task,
                                    1) != pdPASS) {
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
    
    if (g_display_vsync_sem) {
        xSemaphoreGive(g_display_vsync_sem);
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
// Main render task
// ============================================================================

void display_render_task(void *arg)
{
    (void)arg;

    const bool use_vsync = (g_display_buffer_count > 1) && (g_display_vsync_sem != NULL);
    const uint8_t buffer_count = (g_display_buffer_count == 0) ? 1 : g_display_buffer_count;
    int64_t frame_processing_start_us = 0;

#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
    if (use_vsync) {
        xSemaphoreTake(g_display_vsync_sem, 0);
    }
#endif

    while (true) {
        display_render_mode_t mode = g_display_mode_request;
        g_display_mode_active = mode;
        
#if !CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
        if (use_vsync && g_display_mode_request != DISPLAY_RENDER_MODE_UI) {
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
            mode = g_display_mode_request;
            g_display_mode_active = mode;
        }
#endif

        const bool ui_mode = (mode == DISPLAY_RENDER_MODE_UI);

        uint8_t back_buffer_idx = g_render_buffer_index;
        uint8_t *back_buffer = g_display_buffers[back_buffer_idx];
        
        if (!back_buffer) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        frame_processing_start_us = esp_timer_get_time();
        
        int frame_delay_ms = 100;
        uint32_t prev_frame_delay_ms = g_target_frame_delay_ms;

        if (ui_mode) {
            frame_delay_ms = ugfx_ui_render_to_buffer(back_buffer, g_display_row_stride);
            if (frame_delay_ms < 0) {
                memset(back_buffer, 0, g_display_buffer_bytes);
                frame_delay_ms = 100;
            }
            g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
        } else {
            display_frame_callback_t callback = g_display_frame_callback;
            void *ctx = g_display_frame_callback_ctx;
            
            if (callback) {
                prev_frame_delay_ms = g_target_frame_delay_ms;
                frame_delay_ms = callback(back_buffer, ctx);
                if (frame_delay_ms < 0) {
                    back_buffer_idx = g_last_display_buffer;
                    if (back_buffer_idx >= buffer_count) back_buffer_idx = 0;
                    back_buffer = g_display_buffers[back_buffer_idx];
                    frame_delay_ms = 100;
                }
                g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
            } else {
                memset(back_buffer, 0, g_display_buffer_bytes);
                frame_delay_ms = 100;
                g_target_frame_delay_ms = 100;
            }
        }

        // FPS overlay (from display_fps_overlay.c)
        fps_update_and_draw(back_buffer);

        // Processing notification overlay (only in animation mode)
        if (!ui_mode) {
            processing_notification_update_and_draw(back_buffer);
        }

#if DISPLAY_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
        esp_cache_msync(back_buffer, g_display_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif

        g_last_display_buffer = back_buffer_idx;
        g_render_buffer_index = (back_buffer_idx + 1) % buffer_count;

        // Max speed playback: skip frame timing delays when enabled
        if (!config_store_get_max_speed_playback()) {
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

        if (app_lcd_get_brightness() == 0) {
            memset(back_buffer, 0, g_display_buffer_bytes);
#if DISPLAY_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
            esp_cache_msync(back_buffer, g_display_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        }

        esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);

#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
        if (use_vsync && g_display_mode_request != DISPLAY_RENDER_MODE_UI) {
            xSemaphoreTake(g_display_vsync_sem, 0);
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
        }
#endif

        g_last_frame_present_us = esp_timer_get_time();

        if (!use_vsync || g_display_mode_request == DISPLAY_RENDER_MODE_UI) {
            vTaskDelay(1);
        }
    }
}
