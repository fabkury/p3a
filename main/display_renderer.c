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

// Triple buffering state tracking
buffer_info_t g_buffer_info[3] = {
    { .state = BUFFER_STATE_FREE },
    { .state = BUFFER_STATE_FREE },
    { .state = BUFFER_STATE_FREE }
};
volatile int8_t g_displaying_idx = -1;
volatile int8_t g_last_submitted_idx = -1;
SemaphoreHandle_t g_buffer_free_sem = NULL;

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

    // Initialize buffer state tracking
    for (int i = 0; i < buffer_count && i < 3; i++) {
        g_buffer_info[i].state = BUFFER_STATE_FREE;
    }
    g_displaying_idx = -1;
    g_last_submitted_idx = -1;

    // Create semaphore for triple buffering (3+ buffers)
    if (buffer_count >= 3) {
        g_buffer_free_sem = xSemaphoreCreateCounting(buffer_count, buffer_count);
        if (!g_buffer_free_sem) {
            ESP_LOGE(DISPLAY_TAG, "Failed to create buffer-free semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

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

    // Clean up triple buffering state
    if (g_buffer_free_sem) {
        vSemaphoreDelete(g_buffer_free_sem);
        g_buffer_free_sem = NULL;
    }
    for (int i = 0; i < 3; i++) {
        g_buffer_info[i].state = BUFFER_STATE_FREE;
    }
    g_displaying_idx = -1;
    g_last_submitted_idx = -1;

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
    BaseType_t higher_prio_task_woken = pdFALSE;

    // Triple buffering state tracking (when 3+ buffers)
    if (g_display_buffer_count >= 3) {
        // Free the buffer that was displaying
        int8_t prev_displaying = g_displaying_idx;

        // The submitted buffer becomes the displaying buffer
        if (g_last_submitted_idx >= 0 && g_last_submitted_idx < g_display_buffer_count) {
            g_displaying_idx = g_last_submitted_idx;
            g_buffer_info[g_displaying_idx].state = BUFFER_STATE_DISPLAYING;
        }

        // Free the previous displaying buffer (if different)
        if (prev_displaying >= 0 && prev_displaying < g_display_buffer_count &&
            prev_displaying != g_displaying_idx) {
            g_buffer_info[prev_displaying].state = BUFFER_STATE_FREE;
            if (g_buffer_free_sem) {
                xSemaphoreGiveFromISR(g_buffer_free_sem, &higher_prio_task_woken);
            }
        }
    }

    // Legacy semaphore for 2-buffer mode compatibility
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    if (sem) {
        xSemaphoreGiveFromISR(sem, &higher_prio_task_woken);
    }
    return higher_prio_task_woken == pdTRUE;
}

// ============================================================================
// Triple buffering helpers
// ============================================================================

/**
 * @brief Acquire a FREE buffer for rendering (triple buffering mode)
 * @param timeout_ms Maximum time to wait, or portMAX_DELAY for indefinite
 * @return Buffer index (0-2) or -1 if none available
 */
static int8_t acquire_free_buffer(uint32_t timeout_ms)
{
    TickType_t wait_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    TickType_t start_tick = xTaskGetTickCount();

    while (true) {
        // Check for any FREE buffer
        for (int i = 0; i < g_display_buffer_count; i++) {
            if (g_buffer_info[i].state == BUFFER_STATE_FREE) {
                g_buffer_info[i].state = BUFFER_STATE_RENDERING;
                return (int8_t)i;
            }
        }

        if (timeout_ms == 0) return -1;

        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (timeout_ms != portMAX_DELAY && elapsed >= wait_ticks) return -1;

        // Wait for a buffer to become free
        TickType_t remaining = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : (wait_ticks - elapsed);
        xSemaphoreTake(g_buffer_free_sem, remaining);
    }
}

// ============================================================================
// Main render task
// ============================================================================

void display_render_task(void *arg)
{
    (void)arg;

    const bool use_vsync = (g_display_buffer_count > 1) && (g_display_vsync_sem != NULL);
    const uint8_t buffer_count = (g_display_buffer_count == 0) ? 1 : g_display_buffer_count;
    const bool use_triple_buffering = (g_display_buffer_count >= 3) && (g_buffer_free_sem != NULL);
    int64_t frame_processing_start_us = 0;

#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
    // Legacy 2-buffer mode: clear semaphore at start
    if (use_vsync && !use_triple_buffering) {
        xSemaphoreTake(g_display_vsync_sem, 0);
    }
#endif

    while (true) {
        display_render_mode_t mode = g_display_mode_request;
        g_display_mode_active = mode;

        const bool ui_mode = (mode == DISPLAY_RENDER_MODE_UI);

        // ================================================================
        // 1. Acquire buffer
        // ================================================================
        int8_t back_buffer_idx;
        uint8_t *back_buffer;

        if (use_triple_buffering && !ui_mode) {
            // Triple buffering: find any FREE buffer
            back_buffer_idx = acquire_free_buffer(portMAX_DELAY);
            if (back_buffer_idx < 0) {
                // Should not happen with portMAX_DELAY, but handle gracefully
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            back_buffer = g_display_buffers[back_buffer_idx];
        } else {
            // Legacy 2-buffer mode or UI mode: use rotating index with VSYNC wait
#if !CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
            if (use_vsync && !ui_mode) {
                xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
                mode = g_display_mode_request;
                g_display_mode_active = mode;
            }
#endif
            back_buffer_idx = (int8_t)g_render_buffer_index;
            back_buffer = g_display_buffers[back_buffer_idx];
        }

        if (!back_buffer) {
            if (use_triple_buffering && back_buffer_idx >= 0) {
                g_buffer_info[back_buffer_idx].state = BUFFER_STATE_FREE;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        frame_processing_start_us = esp_timer_get_time();

        // ================================================================
        // 2. Render frame via callback
        // ================================================================
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
                    // Callback returned error - reuse last displayed buffer
                    if (use_triple_buffering) {
                        g_buffer_info[back_buffer_idx].state = BUFFER_STATE_FREE;
                    }
                    back_buffer_idx = (int8_t)g_last_display_buffer;
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

        // ================================================================
        // 3. Apply overlays
        // ================================================================
        fps_update_and_draw(back_buffer);

        if (!ui_mode) {
            processing_notification_update_and_draw(back_buffer);
        }

        // ================================================================
        // 4. Cache flush
        // ================================================================
#if DISPLAY_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
        esp_cache_msync(back_buffer, g_display_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif

        // ================================================================
        // 5. Frame timing delay (if not max_speed_playback)
        // ================================================================
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

        // Handle brightness = 0 (black screen)
        if (app_lcd_get_brightness() == 0) {
            memset(back_buffer, 0, g_display_buffer_bytes);
#if DISPLAY_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
            esp_cache_msync(back_buffer, g_display_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        }

        // ================================================================
        // 6. Submit to DMA
        // ================================================================
        g_last_display_buffer = (uint8_t)back_buffer_idx;

        if (use_triple_buffering && !ui_mode) {
            // Check if there's already a pending buffer - if so, wait for VSYNC
            bool has_pending = false;
            for (int i = 0; i < g_display_buffer_count; i++) {
                if (g_buffer_info[i].state == BUFFER_STATE_PENDING) {
                    has_pending = true;
                    break;
                }
            }

            if (has_pending) {
                // Wait for VSYNC to promote the pending buffer to displaying
                xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
            }

            // Now safe to submit - at most 1 buffer will be PENDING
            g_buffer_info[back_buffer_idx].state = BUFFER_STATE_PENDING;
            g_last_submitted_idx = back_buffer_idx;
        } else {
            // Legacy mode: update rotating index
            g_render_buffer_index = ((uint8_t)back_buffer_idx + 1) % buffer_count;
        }

        esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);

        // ================================================================
        // 7. Post-submit handling
        // ================================================================
        if (use_triple_buffering && !ui_mode) {
            // Clear any VSYNC signal that arrived during our submission
            xSemaphoreTake(g_display_vsync_sem, 0);
        } else {
            // Legacy 2-buffer mode: wait for VSYNC
#if CONFIG_P3A_DISPLAY_WAIT_AFTER_DRAW
            if (use_vsync && !ui_mode) {
                xSemaphoreTake(g_display_vsync_sem, 0);
                xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
            }
#endif
        }

        g_last_frame_present_us = esp_timer_get_time();

        if (!use_vsync || ui_mode) {
            vTaskDelay(1);
        }
    }
}
