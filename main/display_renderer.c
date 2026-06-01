// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

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
TaskHandle_t g_display_producer_task = NULL;
TaskHandle_t g_display_consumer_task = NULL;

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

// Multi-buffering state tracking (supports up to 3 buffers)
buffer_info_t g_buffer_info[P3A_MAX_DISPLAY_BUFFERS] = {
    { .state = BUFFER_STATE_FREE },
    { .state = BUFFER_STATE_FREE },
    { .state = BUFFER_STATE_FREE }
};
volatile int8_t g_displaying_idx = -1;
volatile int8_t g_last_submitted_idx = -1;
SemaphoreHandle_t g_buffer_free_sem = NULL;

// Producer -> consumer ready-frame hand-off queue (see ready_frame_t).
QueueHandle_t g_ready_queue = NULL;

// Content generation: bumped on any discontinuity (mode switch, pause/brightness
// transition, artwork swap) so the consumer can drop frames the producer rendered
// for a now-superseded epoch. See display_renderer_note_content_discontinuity().
static volatile uint32_t g_render_generation = 0;

// Timing
int64_t g_last_frame_present_us = 0;
// Wall-clock timestamp of the most recent on_refresh_done event, captured in ISR
// context. Used by the render task to phase-lock submits to vsync edges and to
// distinguish "fresh" vsync signals from cached ones in the binary semaphore.
volatile int64_t g_last_vsync_us = 0;

// Panel vsync period. The current panel is configured at 60 Hz
// (ST7703_720_720_PANEL_60HZ_DPI_CONFIG, see managed_components/.../esp32_p4_wifi6_touch_lcd_4b.c)
// so one refresh = 1e6 / 60 ≈ 16667 us. If the panel config changes, update this.
#define VSYNC_PERIOD_US 16667

// Virtual-playhead accumulator (see Frame timing block in display_consumer_task).
// File-scope so it persists across iterations.
static int64_t s_target_present_us = 0;

// If wall clock drifts more than this from the virtual playhead in either
// direction (ahead or behind), re-baseline rather than chasing. 250 ms was
// picked as a balance: large enough to absorb a slow decode + SD-I/O burst
// without resync chatter, small enough that a real stall doesn't visibly
// fast-forward a long animation when we resume.
#define FRAME_TIMING_RESYNC_US 250000

// Screen rotation
display_rotation_t g_screen_rotation = DISPLAY_ROTATION_0;
volatile bool g_rotation_in_progress = false;

// Processing notification state
volatile proc_notif_state_t g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
volatile int64_t g_proc_notif_start_time_us = 0;
volatile int64_t g_proc_notif_fail_time_us = 0;

// Reaction overlay state
volatile reaction_overlay_state_t g_reaction_overlay_state = REACTION_OVERLAY_IDLE;
volatile int64_t g_reaction_overlay_start_us = 0;

// Pin overlay state (independent from reaction overlay)
volatile pin_overlay_state_t g_pin_overlay_state = PIN_OVERLAY_IDLE;
volatile int64_t g_pin_overlay_start_us = 0;

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
    for (int i = 0; i < buffer_count && i < P3A_MAX_DISPLAY_BUFFERS; i++) {
        g_buffer_info[i].state = BUFFER_STATE_FREE;
    }
    g_displaying_idx = -1;
    g_last_submitted_idx = -1;

    // Create semaphore for triple buffering
    g_buffer_free_sem = xSemaphoreCreateCounting(buffer_count, buffer_count);
    if (!g_buffer_free_sem) {
        ESP_LOGE(DISPLAY_TAG, "Failed to create buffer-free semaphore");
        return ESP_ERR_NO_MEM;
    }

    // Ready-frame hand-off queue (producer -> consumer). Depth covers the at most
    // one banked frame plus headroom; the real in-flight cap is g_buffer_free_sem.
    g_ready_queue = xQueueCreate(2, sizeof(ready_frame_t));
    if (!g_ready_queue) {
        ESP_LOGE(DISPLAY_TAG, "Failed to create ready-frame queue");
        return ESP_ERR_NO_MEM;
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
    // Delete the producer first (stops new frames being enqueued), then the
    // consumer (stops dequeue/submit), then the queue once both readers are gone.
    if (g_display_producer_task) {
        vTaskDelete(g_display_producer_task);
        g_display_producer_task = NULL;
    }

    if (g_display_consumer_task) {
        vTaskDelete(g_display_consumer_task);
        g_display_consumer_task = NULL;
    }

    if (g_ready_queue) {
        vQueueDelete(g_ready_queue);
        g_ready_queue = NULL;
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
    for (int i = 0; i < P3A_MAX_DISPLAY_BUFFERS; i++) {
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
    // Consumer first: it blocks harmlessly on the empty ready-queue until the
    // producer enqueues. Both pinned to core 1 — the consumer must hit vsync
    // deadlines (priority +1 so it preempts the heavy producer), and the producer
    // co-locates with the bottom upscale worker it blocks on.
    if (g_display_consumer_task == NULL) {
        if (xTaskCreatePinnedToCore(display_consumer_task,
                                    "display_consumer",
                                    4096,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY + 1,
                                    &g_display_consumer_task,
                                    1) != pdPASS) {
            ESP_LOGE(DISPLAY_TAG, "Failed to start display consumer task");
            return ESP_FAIL;
        }
    }

    if (g_display_producer_task == NULL) {
        if (xTaskCreatePinnedToCore(display_producer_task,
                                    "display_producer",
                                    4096,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &g_display_producer_task,
                                    1) != pdPASS) {
            ESP_LOGE(DISPLAY_TAG, "Failed to start display producer task");
            if (g_display_consumer_task) {
                vTaskDelete(g_display_consumer_task);
                g_display_consumer_task = NULL;
            }
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

void display_renderer_note_content_discontinuity(void)
{
    // Bump the content generation. The producer stamps subsequent frames with the
    // new value; the consumer drops any queued frame from an older generation
    // instead of presenting it (flush-on-change).
    __atomic_add_fetch(&g_render_generation, 1, __ATOMIC_SEQ_CST);
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
    
    // Nudge the producer in case it is blocked acquiring a free buffer, so it
    // promptly loops, re-reads g_display_mode_request, and flips
    // g_display_mode_active (which wait_for_render_mode polls). The mode flip
    // itself bumps the content generation, flushing stale animation frames.
    if (g_buffer_free_sem) {
        xSemaphoreGive(g_buffer_free_sem);
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreGive(g_buffer_free_sem);
    }
    
    wait_for_render_mode(DISPLAY_RENDER_MODE_UI);
    ESP_LOGI(DISPLAY_TAG, "UI mode active");
    return ESP_OK;
}

void display_renderer_exit_ui_mode(void)
{
    ESP_LOGI(DISPLAY_TAG, "Exiting UI mode");
    g_display_mode_request = DISPLAY_RENDER_MODE_ANIMATION;

    // Nudge the producer (see enter_ui_mode) so it re-reads the mode promptly.
    if (g_buffer_free_sem) {
        xSemaphoreGive(g_buffer_free_sem);
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

bool display_panel_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                   esp_lcd_dpi_panel_event_data_t *edata,
                                   void *user_ctx)
{
    (void)panel;
    (void)edata;
    BaseType_t higher_prio_task_woken = pdFALSE;

    // Stamp the moment this vsync event fired. esp_timer_get_time() is documented
    // ISR-safe. The render task uses this to detect whether a semaphore signal it
    // just consumed is fresh (just now) or cached (fired earlier and queued).
    g_last_vsync_us = esp_timer_get_time();

    // Triple buffering state tracking
    {
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

    // VSYNC signaling for render task
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
// Producer task: acquire a buffer, render (decode + upscale + overlays), enqueue
// ============================================================================

void display_producer_task(void *arg)
{
    (void)arg;

    // Tracks the previous "black output" state (paused or brightness-zero). A
    // transition bumps the content generation so the consumer drops queued
    // frames from the superseded epoch (e.g. pause should freeze immediately).
    bool prev_black = false;

    while (true) {
        display_render_mode_t mode = g_display_mode_request;
        if (mode != g_display_mode_active) {
            // Mode switch: invalidate frames queued for the old mode.
            display_renderer_note_content_discontinuity();
        }
        g_display_mode_active = mode;

        const bool ui_mode = (mode == DISPLAY_RENDER_MODE_UI);

        // ================================================================
        // 1. Acquire a free buffer to render into
        // ================================================================
        int8_t back_buffer_idx = acquire_free_buffer(portMAX_DELAY);
        if (back_buffer_idx < 0) {
            // Should not happen with portMAX_DELAY, but handle gracefully
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        uint8_t *back_buffer = g_display_buffers[back_buffer_idx];

        if (!back_buffer) {
            g_buffer_info[back_buffer_idx].state = BUFFER_STATE_FREE;
            if (g_buffer_free_sem) {
                xSemaphoreGive(g_buffer_free_sem);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // ================================================================
        // 2. Render the frame (mode branch)
        // ================================================================
        int frame_delay_ms = 100;

        bool brightness_zero = (app_lcd_get_brightness() == 0);
        bool anim_paused = app_lcd_is_animation_paused();
        bool black = anim_paused || brightness_zero;
        if (black != prev_black) {
            // Entering or leaving black output: drop queued frames so the
            // change lands on the next vsync.
            display_renderer_note_content_discontinuity();
            prev_black = black;
        }

        if (brightness_zero && !anim_paused) {
            // User manually set brightness to 0 (not paused): skip callback
            memset(back_buffer, 0, g_display_buffer_bytes);
            frame_delay_ms = 100;
        } else if (anim_paused) {
            // Paused: always output black regardless of render mode
            memset(back_buffer, 0, g_display_buffer_bytes);
            frame_delay_ms = 100;
        } else {
            if (ui_mode) {
                frame_delay_ms = ugfx_ui_render_to_buffer(back_buffer, g_display_row_stride);
                if (frame_delay_ms < 0) {
                    memset(back_buffer, 0, g_display_buffer_bytes);
                    frame_delay_ms = 100;
                }
            } else {
                display_frame_callback_t callback = g_display_frame_callback;
                void *ctx = g_display_frame_callback_ctx;

                if (callback) {
                    frame_delay_ms = callback(back_buffer, ctx);
                    if (frame_delay_ms < 0) {
                        // Callback error: release the buffer and skip this frame.
                        // The consumer never sees it; DMA keeps showing the last
                        // submitted frame.
                        g_buffer_info[back_buffer_idx].state = BUFFER_STATE_FREE;
                        if (g_buffer_free_sem) {
                            xSemaphoreGive(g_buffer_free_sem);
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                } else {
                    memset(back_buffer, 0, g_display_buffer_bytes);
                    frame_delay_ms = 100;
                }
            }

            // ================================================================
            // 3. Bake overlays into the frame (before it is queued)
            // ================================================================
            fps_update_and_draw(back_buffer);

            if (!ui_mode) {
                processing_notification_update_and_draw(back_buffer);
                reaction_overlay_update_and_draw(back_buffer);
                pin_overlay_update_and_draw(back_buffer);
            }
        }

        // ================================================================
        // 4. Hand the finished frame to the consumer
        // ================================================================
        g_buffer_info[back_buffer_idx].state = BUFFER_STATE_READY;
        ready_frame_t rf = {
            .buffer_idx  = back_buffer_idx,
            .duration_ms = (uint32_t)frame_delay_ms,
            .generation  = __atomic_load_n(&g_render_generation, __ATOMIC_SEQ_CST),
        };
        xQueueSend(g_ready_queue, &rf, portMAX_DELAY);
    }
}

// ============================================================================
// Consumer task: dequeue, drop stale frames, pace to the playhead, submit
// ============================================================================

void display_consumer_task(void *arg)
{
    (void)arg;

    // Generation of the frame most recently presented. A change marks a content
    // discontinuity, so the next presented frame is baselined to "now".
    uint32_t last_presented_gen = UINT32_MAX;

    while (true) {
        // ================================================================
        // 1. Dequeue the next ready frame
        // ================================================================
        ready_frame_t rf;
        if (xQueueReceive(g_ready_queue, &rf, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int8_t back_buffer_idx = rf.buffer_idx;
        uint8_t *back_buffer = g_display_buffers[back_buffer_idx];

        // Flush-on-change: drop frames the producer rendered for a superseded
        // content generation. The buffer returns straight to the free pool;
        // nothing is presented and the playhead is left untouched, so DMA keeps
        // showing the last submitted frame until a current-generation frame
        // arrives.
        const uint32_t cur_gen = __atomic_load_n(&g_render_generation, __ATOMIC_SEQ_CST);
        if (rf.generation != cur_gen) {
            g_buffer_info[back_buffer_idx].state = BUFFER_STATE_FREE;
            if (g_buffer_free_sem) {
                xSemaphoreGive(g_buffer_free_sem);
            }
            continue;
        }

        // First frame of a new generation: present it immediately rather than on
        // the stale playhead schedule (baseline handled in the timing block).
        if (rf.generation != last_presented_gen) {
            g_last_frame_present_us = 0;
            last_presented_gen = rf.generation;
        }

        const bool max_speed = config_store_get_max_speed_playback();

        // ================================================================
        // 2. Frame timing — virtual playhead with fractional vsync alignment
        // ================================================================
        //
        // The playhead advances by THIS frame's carried duration AFTER it is
        // submitted (end of loop), so on entry s_target_present_us already holds
        // this frame's intended present time. Duration accumulates exactly once
        // per presented frame, so long-term drift is zero even when source
        // delays aren't multiples of the panel's vsync period (16.67 ms at
        // 60 Hz). The half-period vsync rounding below gives the fractional
        // alignment that makes 50 fps content average 20 ms (alternating
        // 16.67 / 33.33) rather than ceiling-quantizing to 33.33 every frame.
        //
        if (!max_speed) {
            const int64_t now_us = esp_timer_get_time();

            if (g_last_frame_present_us == 0) {
                // First frame after init / discontinuity / long pause: baseline
                // the playhead to "now" so we present immediately.
                s_target_present_us = now_us;
            } else {
                // Drift safeguard — POSITIVE direction only.
                //
                // Positive drift (now ran ahead of target by more than the
                // threshold) means a stall (slow decode, SD-I/O burst, OS
                // preemption) cost so much time that this frame's intended slot
                // is already in the past. Resync forfeits the lost time and
                // resumes from "now" rather than submitting a catch-up burst
                // that would visibly fast-forward the animation.
                //
                // NEGATIVE drift (target > now) is the NORMAL state for any
                // frame whose duration exceeds one loop iteration: the sleep
                // below waits until the playhead catches up. We MUST NOT resync
                // there — doing so would spin the loop at the vsync rate (the
                // original 60-fps-pin bug).
                int64_t drift_us = now_us - s_target_present_us;
                if (drift_us > FRAME_TIMING_RESYNC_US) {
                    s_target_present_us = now_us;
                }
            }

            // Sleep until ~1.5 ms before target, leaving slack for the alignment
            // loop. Trivial residuals (<3 ms) skip the sleep (FreeRTOS tick =
            // 1 ms at CONFIG_FREERTOS_HZ=1000) and block on the vsync sem below.
            int64_t residual_us = s_target_present_us - esp_timer_get_time();
            if (residual_us > 3000) {
                int64_t sleep_us = residual_us - 1500;
                vTaskDelay(pdMS_TO_TICKS((sleep_us + 500) / 1000));
            }
        }

        // ================================================================
        // 3. Vsync alignment + submit
        // ================================================================
        if (max_speed) {
            // Max-speed: consume one vsync per submit (no playhead, no sleep).
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
        } else {
            // Phase-lock the submit to the vsync edge nearest the playhead:
            // take a vsync signal, read the ISR-stamped g_last_vsync_us, and if
            // that edge is within half a period of (or past) the target, present
            // here; otherwise wait for the next edge.
            //
            // Tearing safety: if take() returned a cached signal whose edge is
            // already more than ~half a vsync old, "now" is well into the next
            // refresh and submitting would tear, so skip it and wait for a fresh
            // vsync (the accumulator compensates next frame).
            const int64_t cached_age_threshold_us = VSYNC_PERIOD_US / 2;
            while (true) {
                xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
                const int64_t vsync_us = g_last_vsync_us;
                const int64_t now_us = esp_timer_get_time();
                const bool cached_too_late = (now_us - vsync_us) > cached_age_threshold_us;

                if (vsync_us + (VSYNC_PERIOD_US / 2) >= s_target_present_us) {
                    if (!cached_too_late) {
                        break;  // present this vsync edge
                    }
                    // else: cached signal stale, refresh is already past;
                    // wait one more vsync to land in the next vblank.
                }
                // else: vsync edge more than half a period before target;
                // wait for the next one.
            }
        }

        // Now safe to submit - at most 1 buffer will be PENDING
        g_buffer_info[back_buffer_idx].state = BUFFER_STATE_PENDING;
        g_last_submitted_idx = back_buffer_idx;

        esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);

        g_last_frame_present_us = esp_timer_get_time();

        // Advance the playhead by THIS frame's intended duration so the next
        // frame is due exactly that long after this one was presented.
        if (!max_speed) {
            s_target_present_us += (int64_t)rf.duration_ms * 1000;
        }
    }
}
