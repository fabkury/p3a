// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The display pipeline's core: framebuffer ownership, the producer and the
// consumer, vsync. See display_renderer.h.
//
// From "Vsync setup" down to the end of the consumer, this file is the 1.x
// code as the jitter work left it (docs/jitter/REPORT.md). Do not change the
// timing there without re-measuring against the pass bar.

#include "display_renderer_priv.h"

#include <stdatomic.h>
#include <string.h>

#include "p3a_event.h"
#include "p3a_settings.h"
#include "p3a_task.h"

// LCD panel state
esp_lcd_panel_handle_t g_display_panel = NULL;
static uint8_t *s_buffers[P3A_MAX_DISPLAY_BUFFERS];
uint8_t **g_display_buffers = s_buffers;
uint8_t g_display_buffer_count = 0;
size_t g_display_buffer_bytes = 0;
size_t g_display_row_stride = 0;

// Synchronization
SemaphoreHandle_t g_display_vsync_sem = NULL;
TaskHandle_t g_display_producer_task = NULL;
TaskHandle_t g_display_consumer_task = NULL;

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

// Content generation: bumped on any discontinuity (source switch, blank
// transition, artwork swap) so the consumer can drop frames the producer rendered
// for a now-superseded epoch. See display_renderer_note_content_discontinuity().
static volatile uint32_t g_render_generation = 0;

// Timing
int64_t g_last_frame_present_us = 0;
// Wall-clock timestamp of the most recent on_refresh_done event, captured in ISR
// context. Used by the render task to phase-lock submits to vsync edges and to
// distinguish "fresh" vsync signals from cached ones in the binary semaphore.
volatile int64_t g_last_vsync_us = 0;

// Panel vsync period. The panel is configured at 60 Hz
// (ST7703_720_720_PANEL_60HZ_DPI_CONFIG in the board support package), so one
// refresh = 1e6 / 60 = 16667 us. If the panel config changes, update this.
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

// --- what the settings say, cached: nothing here reads a setting per frame ---
static atomic_int s_rotation = DISPLAY_ROTATION_0;
static atomic_bool s_max_speed;
static atomic_uint s_background;  // 0xRRGGBB
atomic_bool g_display_show_fps;

// --- sources and overlays ---
typedef struct {
    display_frame_source_fn fn;
    void *ctx;
} source_t;

typedef struct {
    display_overlay_fn fn;
    void *ctx;
    bool over_ui;
} overlay_t;

#define MAX_OVERLAYS 6

static SemaphoreHandle_t s_registry_lock;  // guards the two tables below
static source_t s_sources[DISPLAY_SOURCE_COUNT_];
static overlay_t s_overlays[MAX_OVERLAYS];
static int s_overlay_count;
static volatile display_source_t s_source_request = DISPLAY_SOURCE_ARTWORK;
static volatile display_source_t s_source_active = DISPLAY_SOURCE_ARTWORK;
static atomic_bool s_blank;

// Forward declarations
static esp_err_t prepare_vsync(void);
static esp_err_t create_upscale_workers(void);

// ============================================================================
// Settings
// ============================================================================

static void take_settings(void)
{
    static p3a_settings_t settings;  // too large for a task stack
    p3a_settings_get(&settings);
    if (atomic_exchange(&s_rotation, settings.rotation) != settings.rotation) {
        ESP_LOGI(DISPLAY_TAG, "rotation %d degrees", (int)settings.rotation);
        // The picture on the panel is for the old orientation.
        display_renderer_note_content_discontinuity();
    }
    atomic_store(&s_max_speed, settings.playback_max_speed);
    atomic_store(&s_background, settings.background_color);
    atomic_store(&g_display_show_fps, settings.show_fps);
}

static void on_setting_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    switch (event->payload.i32) {
    case P3A_SET_ROTATION:
    case P3A_SET_PLAYBACK_MAX_SPEED:
    case P3A_SET_BACKGROUND_COLOR:
    case P3A_SET_SHOW_FPS:
        take_settings();
        break;
    default:
        break;
    }
}

display_rotation_t display_renderer_get_rotation(void)
{
    return (display_rotation_t)atomic_load(&s_rotation);
}

void display_renderer_get_background(uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned rgb = atomic_load(&s_background);
    *r = (uint8_t)(rgb >> 16);
    *g = (uint8_t)(rgb >> 8);
    *b = (uint8_t)rgb;
}

size_t display_renderer_row_stride(void)
{
    return g_display_row_stride;
}

size_t display_renderer_buffer_bytes(void)
{
    return g_display_buffer_bytes;
}

const uint8_t *display_renderer_displayed_buffer(void)
{
    int8_t index = g_displaying_idx;
    return index >= 0 && index < g_display_buffer_count ? g_display_buffers[index] : NULL;
}

// ============================================================================
// Start
// ============================================================================

esp_err_t display_renderer_ensure_upscale_workers(void)
{
    if (g_upscale_worker_top && g_upscale_worker_bottom) {
        return ESP_OK;
    }
    return create_upscale_workers();
}

// One worker per core, at the producer's priority. Their stacks are internal
// RAM, as the RENDER class says: they run every frame.
static esp_err_t create_upscale_workers(void)
{
    if (!g_upscale_worker_top) {
        p3a_task_config_t top = P3A_TASK_CONFIG("upscale_top", display_upscale_worker_top_task, NULL, 2048, P3A_TASK_RENDER);
        top.core = 0;
        esp_err_t err = p3a_task_create(&top, &g_upscale_worker_top);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!g_upscale_worker_bottom) {
        p3a_task_config_t bottom =
            P3A_TASK_CONFIG("upscale_bottom", display_upscale_worker_bottom_task, NULL, 2048, P3A_TASK_RENDER);
        bottom.core = 1;
        esp_err_t err = p3a_task_create(&bottom, &g_upscale_worker_bottom);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t display_renderer_start(void)
{
    if (g_display_consumer_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    g_display_panel = p3a_board_get_panel();
    g_display_buffer_count = p3a_board_get_buffer_count();
    g_display_buffer_bytes = p3a_board_get_buffer_bytes();
    g_display_row_stride = p3a_board_get_row_stride();
    if (g_display_panel == NULL || g_display_buffer_count == 0 || g_display_buffer_count > P3A_MAX_DISPLAY_BUFFERS) {
        ESP_LOGE(DISPLAY_TAG, "no panel, or an unexpected number of framebuffers (%u)", (unsigned)g_display_buffer_count);
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < g_display_buffer_count; i++) {
        s_buffers[i] = p3a_board_get_buffer(i);
        g_buffer_info[i].state = BUFFER_STATE_FREE;
    }
    g_displaying_idx = -1;
    g_last_submitted_idx = -1;

    g_buffer_free_sem = xSemaphoreCreateCounting(g_display_buffer_count, g_display_buffer_count);
    // Ready-frame hand-off queue (producer -> consumer). Depth covers the at most
    // one banked frame plus headroom; the real in-flight cap is g_buffer_free_sem.
    g_ready_queue = xQueueCreate(2, sizeof(ready_frame_t));
    s_registry_lock = xSemaphoreCreateMutex();
    if (!g_buffer_free_sem || !g_ready_queue || !s_registry_lock) {
        return ESP_ERR_NO_MEM;
    }

    take_settings();
    esp_err_t err = p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting_changed, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(DISPLAY_TAG, "settings will not be followed: %s", esp_err_to_name(err));
    }
    display_fps_overlay_register();

    err = prepare_vsync();
    if (err != ESP_OK) {
        return err;
    }

    // Consumer first: it blocks harmlessly on the empty ready-queue until the
    // producer enqueues. Both on the render core: the consumer must hit vsync
    // deadlines (one priority step up, so that it preempts the heavy producer),
    // and the producer shares a core with the bottom upscale worker it waits for.
    p3a_task_config_t consumer = P3A_TASK_CONFIG("display_consumer", display_consumer_task, NULL, 4096, P3A_TASK_RENDER);
    consumer.priority_offset = 1;
    p3a_task_config_t producer = P3A_TASK_CONFIG("display_producer", display_producer_task, NULL, 4096, P3A_TASK_RENDER);
    if ((err = p3a_task_create(&consumer, &g_display_consumer_task)) != ESP_OK ||
        (err = p3a_task_create(&producer, &g_display_producer_task)) != ESP_OK) {
        ESP_LOGE(DISPLAY_TAG, "the display tasks did not start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(DISPLAY_TAG, "started: %u framebuffers, rotation %d", (unsigned)g_display_buffer_count,
             (int)display_renderer_get_rotation());
    return ESP_OK;
}

// ============================================================================
// Sources and overlays
// ============================================================================

void display_renderer_set_source(display_source_t slot, display_frame_source_fn fn, void *ctx)
{
    if ((int)slot < 0 || slot >= DISPLAY_SOURCE_COUNT_ || s_registry_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    s_sources[slot].fn = fn;
    s_sources[slot].ctx = ctx;
    xSemaphoreGive(s_registry_lock);
    // Whatever is queued was drawn by the previous owner of the slot.
    display_renderer_note_content_discontinuity();
}

esp_err_t display_renderer_add_overlay(display_overlay_fn fn, void *ctx, bool over_ui)
{
    if (fn == NULL || s_registry_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
    if (s_overlay_count < MAX_OVERLAYS) {
        s_overlays[s_overlay_count] = (overlay_t){.fn = fn, .ctx = ctx, .over_ui = over_ui};
        s_overlay_count++;
        err = ESP_OK;
    }
    xSemaphoreGive(s_registry_lock);
    return err;
}

void display_renderer_note_content_discontinuity(void)
{
    // Bump the content generation. The producer stamps subsequent frames with the
    // new value; the consumer drops any queued frame from an older generation
    // instead of presenting it (flush-on-change).
    uint32_t gen = __atomic_add_fetch(&g_render_generation, 1, __ATOMIC_SEQ_CST);
    (void)gen;
    frame_trace_mark(FT_MARK_SWAP, FT_PHASE_EVENT, gen);
}

void display_renderer_set_blank(bool blank)
{
    atomic_store(&s_blank, blank);
}

display_source_t display_renderer_active_source(void)
{
    return s_source_active;
}

esp_err_t display_renderer_select_source(display_source_t slot)
{
    if ((int)slot < 0 || slot >= DISPLAY_SOURCE_COUNT_) {
        return ESP_ERR_INVALID_ARG;
    }
    s_source_request = slot;

    // Nudge the producer in case it is blocked acquiring a free buffer, so that
    // it loops, reads the request and switches. The switch itself bumps the
    // content generation, flushing the other source's queued frames.
    if (g_buffer_free_sem) {
        xSemaphoreGive(g_buffer_free_sem);
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreGive(g_buffer_free_sem);
    }

    const TickType_t step = pdMS_TO_TICKS(5);
    for (TickType_t waited = 0; s_source_active != slot; waited += step) {
        if (waited >= pdMS_TO_TICKS(500)) {
            ESP_LOGW(DISPLAY_TAG, "timed out waiting for source %d (active %d)", (int)slot, (int)s_source_active);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(step);
    }
    return ESP_OK;
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
// Producer task: acquire a buffer, render (source + overlays), enqueue
// ============================================================================

// Returns the buffer to the free pool without queuing it.
static void release_unqueued(int8_t index)
{
    g_buffer_info[index].state = BUFFER_STATE_FREE;
    if (g_buffer_free_sem) {
        xSemaphoreGive(g_buffer_free_sem);
    }
}

void display_producer_task(void *arg)
{
    (void)arg;

    // Tracks the previous "black output" state (blank or brightness-zero). A
    // transition bumps the content generation so the consumer drops queued
    // frames from the superseded epoch (e.g. pause should freeze immediately).
    bool prev_black = false;
    // True when the frame on the panel is not the active source's own last one.
    bool must_redraw = true;

    while (true) {
        display_source_t slot = s_source_request;
        if (slot != s_source_active) {
            // Source switch: invalidate frames queued by the other source, and
            // make the new one draw a fresh first frame.
            frame_trace_mark(FT_MARK_MODE_SWITCH, FT_PHASE_EVENT, (uint32_t)slot);
            display_renderer_note_content_discontinuity();
            must_redraw = true;
        }
        s_source_active = slot;

        const bool ui_mode = (slot != DISPLAY_SOURCE_ARTWORK);

        // ================================================================
        // 1. Acquire a free buffer to render into
        // ================================================================
#if CONFIG_P3A_FRAME_TRACE
        const int64_t ft_free_t0 = frame_trace_now_us();
#endif
        int8_t back_buffer_idx = acquire_free_buffer(portMAX_DELAY);
#if CONFIG_P3A_FRAME_TRACE
        const int64_t ft_produce_t0 = frame_trace_now_us();
#endif
        if (back_buffer_idx < 0) {
            // Should not happen with portMAX_DELAY, but handle gracefully
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        uint8_t *back_buffer = g_display_buffers[back_buffer_idx];

        if (!back_buffer) {
            release_unqueued(back_buffer_idx);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // ================================================================
        // 2. Render the frame
        // ================================================================
        int frame_delay_ms = 100;

        // Black output: asked for (pause), or the backlight is off anyway.
        bool black = atomic_load(&s_blank) || p3a_board_get_brightness() == 0;
        if (black != prev_black) {
            // Entering or leaving black output: drop queued frames so the
            // change lands on the next vsync. Leaving black also needs a
            // fresh frame from the source (the last presented frame was black).
            display_renderer_note_content_discontinuity();
            must_redraw = true;
            prev_black = black;
        }

        if (black) {
            memset(back_buffer, 0, g_display_buffer_bytes);
            frame_delay_ms = 100;
        } else {
            xSemaphoreTake(s_registry_lock, portMAX_DELAY);
            const source_t source = s_sources[slot];
            xSemaphoreGive(s_registry_lock);

            if (source.fn) {
                frame_delay_ms = source.fn(back_buffer, must_redraw, source.ctx);
                if (frame_delay_ms == DISPLAY_FRAME_UNCHANGED && !must_redraw) {
                    // Nothing new to show: release the buffer without queuing
                    // it, so the panel keeps the last submitted frame, and
                    // idle instead of re-rendering. (In 1.x a screen that
                    // redrew in full every 50 ms took longer than that per
                    // redraw; this task never blocked, the idle task starved,
                    // and the task watchdog's report delayed the vsync
                    // interrupt, which showed as a blue frame.)
                    release_unqueued(back_buffer_idx);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                if (frame_delay_ms < 0) {
                    // The source failed, or answered "unchanged" when a fresh
                    // frame was required: skip this frame. The consumer never
                    // sees it; the panel keeps showing the last one.
                    release_unqueued(back_buffer_idx);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                must_redraw = false;
            } else {
                // Nobody draws for this slot yet.
                memset(back_buffer, 0, g_display_buffer_bytes);
                frame_delay_ms = 100;
            }

            // ================================================================
            // 3. Bake overlays into the frame (before it is queued)
            // ================================================================
            xSemaphoreTake(s_registry_lock, portMAX_DELAY);
            overlay_t overlays[MAX_OVERLAYS];
            const int overlay_count = s_overlay_count;
            memcpy(overlays, s_overlays, sizeof(overlays));
            xSemaphoreGive(s_registry_lock);
            for (int i = 0; i < overlay_count && slot != DISPLAY_SOURCE_TEST; i++) {
                if (!ui_mode || overlays[i].over_ui) {
                    overlays[i].fn(back_buffer, overlays[i].ctx);
                }
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
            .produce_end_us = esp_timer_get_time(),
        };
#if CONFIG_P3A_FRAME_TRACE
        rf.ft_produce_start_us = ft_produce_t0;
        rf.ft_produce_end_us = frame_trace_now_us();
        rf.ft_free_wait_us = (uint32_t)(ft_produce_t0 - ft_free_t0);
        frame_trace_producer_take_split(&rf.ft_decode_us, &rf.ft_upscale_us);
        rf.ft_flags = (uint8_t)((ui_mode ? FT_FLAG_UI_MODE : 0) | (black ? FT_FLAG_BLACK : 0));
#endif
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
#if CONFIG_P3A_FRAME_TRACE
        // Jitter work stream: capture the target BEFORE baseline/resync can rewrite it,
        // so a resynced frame still records its true lateness.
        const uint8_t ft_queue_depth = (uint8_t)uxQueueMessagesWaiting(g_ready_queue);
        const int64_t ft_target_us = s_target_present_us;
        uint8_t ft_flags = rf.ft_flags;
        uint32_t ft_vsync_wait_us = 0;
#endif

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

        const bool max_speed = atomic_load(&s_max_speed);

        // ================================================================
        // 2. Frame timing: virtual playhead with fractional vsync alignment
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
#if CONFIG_P3A_FRAME_TRACE
                ft_flags |= FT_FLAG_BASELINED;
#endif
            } else {
                // Drift safeguard: POSITIVE direction only.
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
                // there: doing so would spin the loop at the vsync rate (the
                // original 60-fps-pin bug).
                int64_t drift_us = now_us - s_target_present_us;
                if (drift_us > FRAME_TIMING_RESYNC_US) {
#if CONFIG_P3A_FRAME_TRACE
                    ft_flags |= FT_FLAG_RESYNCED;
                    frame_trace_mark(FT_MARK_RESYNC, FT_PHASE_EVENT, (uint32_t)drift_us);
#endif
                    s_target_present_us = now_us;
                } else if (drift_us > 0 && rf.produce_end_us > s_target_present_us) {
                    // Producer-bound content (jitter work stream Phase 6 policy,
                    // 2026-08-30): this frame was finished AFTER its intended
                    // present time, so the producer, not a hiccup, is the
                    // bottleneck. Accumulating that deficit only builds the
                    // 250 ms sawtooth above (a visible skip every 0.5-5 s on
                    // artworks the chip cannot decode in time). Re-baseline
                    // instead: present as soon as possible and measure the next
                    // frame from this present time, i.e. a uniform slowdown.
                    // Frames that were ready in time but are presented late
                    // (consumer-side hiccup) keep the catch-up behaviour.
                    s_target_present_us = now_us;
#if CONFIG_P3A_FRAME_TRACE
                    ft_flags |= FT_FLAG_REBASED;
#endif
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
#if CONFIG_P3A_FRAME_TRACE
        const int64_t ft_vs0 = frame_trace_now_us();
        if (max_speed) ft_flags |= FT_FLAG_MAX_SPEED;
#endif
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

#if CONFIG_P3A_FRAME_TRACE
        ft_vsync_wait_us = (uint32_t)(frame_trace_now_us() - ft_vs0);
#endif
        // Now safe to submit - at most 1 buffer will be PENDING
        g_buffer_info[back_buffer_idx].state = BUFFER_STATE_PENDING;
        g_last_submitted_idx = back_buffer_idx;

        esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, back_buffer);

        g_last_frame_present_us = esp_timer_get_time();

#if CONFIG_P3A_FRAME_TRACE
        {
            ft_frame_in_t fin = {
                .target_us        = ft_target_us,
                .present_us       = g_last_frame_present_us,
                .produce_start_us = rf.ft_produce_start_us,
                .produce_end_us   = rf.ft_produce_end_us,
                .decode_us        = rf.ft_decode_us,
                .upscale_us       = rf.ft_upscale_us,
                .free_wait_us     = rf.ft_free_wait_us,
                .vsync_wait_us    = ft_vsync_wait_us,
                .duration_ms      = rf.duration_ms,
                .generation       = rf.generation,
                .queue_depth      = ft_queue_depth,
                .flags            = ft_flags,
            };
            frame_trace_frame(&fin);
        }
#endif

        // Advance the playhead by THIS frame's intended duration so the next
        // frame is due exactly that long after this one was presented.
        if (!max_speed) {
            s_target_present_us += (int64_t)rf.duration_ms * 1000;
        }
    }
}
