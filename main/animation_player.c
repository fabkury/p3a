// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file animation_player.c
 * @brief Animation player core: buffer management, initialization, swap orchestration
 */

#include "animation_player_priv.h"
#include "esp_heap_caps.h"
#include "sd_path.h"
#include "play_scheduler.h"
#include "active_playset_store.h"
#include "psram_alloc.h"
#include "sdcard_channel_impl.h"
#include "playlist_manager.h"
#include "content_cache.h"
#include "ugfx_ui.h"
#include "config_store.h"
#include "ota_manager.h"
#include "sdio_bus.h"
#include "pico8_stream.h"
#include "pico8_render.h"
#include "p3a_state.h"
#include "makapix_channel_impl.h"
#include "makapix.h"
#include "p3a_render.h"
#include "display_renderer.h"
#include "makapix_channel_events.h"
#include "fresh_boot.h"
#include "render_engine.h"
#include "playback_queue.h"
#include "event_bus.h"

// Animation player state
animation_buffer_t s_front_buffer = {0};
animation_buffer_t s_back_buffer = {0};
size_t s_next_asset_index = 0;
bool s_swap_requested = false;
bool s_loader_busy = false;
TaskHandle_t s_loader_task = NULL;
SemaphoreHandle_t s_loader_sem = NULL;
SemaphoreHandle_t s_buffer_mutex = NULL;
SemaphoreHandle_t s_prefetch_done_sem = NULL;

bool s_anim_paused = false;

// Load override (one-shot, set by animation_player_request_swap)
animation_load_override_t s_load_override = {0};

app_lcd_sd_file_list_t s_sd_file_list = {0};
bool s_sd_mounted = false;
bool s_sd_export_active = false;
bool s_sd_access_paused = false;

// PSRAM-backed stack for SD refresh task
static StackType_t *s_sd_refresh_stack = NULL;
static StaticTask_t s_sd_refresh_task_buffer;
static volatile bool s_sd_refresh_task_running = false;

typedef struct {
    TaskHandle_t requester;
    esp_err_t result;
} sd_refresh_request_t;

static void animation_player_sd_refresh_task(void *arg)
{
    sd_refresh_request_t *req = (sd_refresh_request_t *)arg;
    animation_loader_wait_for_idle();
    esp_err_t res = refresh_animation_file_list();
    if (req) {
        req->result = res;
        if (req->requester) {
            xTaskNotifyGive(req->requester);
        }
    }
    s_sd_refresh_task_running = false;
    vTaskDelete(NULL);
}

// Render dispatch: use state-aware renderer when available so channel status messages
// can be drawn without entering display "UI mode" (which may be slow/unreliable on some boots).
static int animation_player_render_dispatch_cb(uint8_t *dest_buffer, void *user_ctx)
{
    (void)user_ctx;
    if (!dest_buffer) return -1;

    size_t stride = 0;
    display_renderer_get_dimensions(NULL, NULL, &stride);

    p3a_render_result_t rr = {0};
    esp_err_t render_err = p3a_render_frame(dest_buffer, stride, &rr);
    
    if (render_err == ESP_OK && rr.buffer_modified) {
        // State-aware renderer succeeded and drew something
        return rr.frame_delay_ms;
    }
    
    // If p3a_render returned OK but didn't modify buffer (e.g., waiting for something),
    // or if it failed entirely, try direct animation rendering
    int anim_delay = animation_player_render_frame_callback(dest_buffer, NULL);
    if (anim_delay >= 0) {
        return anim_delay;
    }
    
    // No animation available - try µGFX UI directly as last resort
    // This ensures channel messages show even if state machine isn't set up correctly
    if (ugfx_ui_is_active()) {
        int ui_delay = ugfx_ui_render_to_buffer(dest_buffer, stride);
        if (ui_delay >= 0) {
            return ui_delay;
        }
    }
    
    // Absolute fallback: return -1 to show last frame or black
    return -1;
}

static esp_err_t mount_sd_and_discover(char **animations_dir_out)
{
    if (!s_sd_mounted) {
        esp_err_t sd_err = bsp_sdcard_mount();
        if (sd_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(sd_err));
            return sd_err;
        }
        s_sd_mounted = true;

#if CONFIG_P3A_FORCE_FRESH_SDCARD
        // Debug: Erase SD card p3a directory to simulate fresh boot
        ESP_LOGW(TAG, "CONFIG_P3A_FORCE_FRESH_SDCARD enabled - erasing /sdcard/p3a");
        fresh_boot_erase_sdcard();
#endif
    }

    // Initialize SD path module (loads configured root from NVS)
    sd_path_init();
    
    // Ensure all required directories exist under the configured root
    esp_err_t dir_err = sd_path_ensure_directories();
    if (dir_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create some SD directories: %s", esp_err_to_name(dir_err));
        // Continue anyway - directories might already exist or be read-only
    }

    // Get the animations directory path
    char animations_path[128];
    esp_err_t path_err = sd_path_get_animations(animations_path, sizeof(animations_path));
    if (path_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get animations path");
        return path_err;
    }

    *animations_dir_out = strdup(animations_path);
    if (!*animations_dir_out) {
        return ESP_ERR_NO_MEM;
    }
    
    if (directory_has_animation_files(animations_path)) {
        ESP_LOGD(TAG, "Using animations directory: %s", animations_path);
    } else {
        ESP_LOGD(TAG, "Animations directory is empty: %s", animations_path);
    }
    
    return ESP_OK;
}

// NOTE: load_first_animation() was removed. Boot playback now uses swap_to(0, 0)
// which goes through the proper swap mechanism.

esp_err_t animation_player_init(esp_lcd_panel_handle_t display_handle,
                                 uint8_t **lcd_buffers,
                                 uint8_t buffer_count,
                                 size_t buffer_bytes,
                                 size_t row_stride_bytes)
{
    if (!display_handle || !lcd_buffers || buffer_count == 0 || buffer_bytes == 0 || row_stride_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize display renderer
    esp_err_t err = display_renderer_init(display_handle, lcd_buffers, buffer_count, buffer_bytes, row_stride_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display renderer: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize playback controller
    err = playback_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize playback controller: %s", esp_err_to_name(err));
        display_renderer_deinit();
        return err;
    }

    // Mount SD card and discover animations directory early (required for playlist + per-channel files)
    char *found_animations_dir = NULL;
    err = mount_sd_and_discover(&found_animations_dir);
    if (err != ESP_OK || !found_animations_dir) {
        ESP_LOGE(TAG, "Failed to find animations directory: %s", esp_err_to_name(err));
        if (found_animations_dir) {
            free(found_animations_dir);
        }
        playback_controller_deinit();
        display_renderer_deinit();
        return (err == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    err = playlist_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize playlist manager: %s", esp_err_to_name(err));
        free(found_animations_dir);
        playback_controller_deinit();
        display_renderer_deinit();
        return err;
    }

    err = content_cache_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize download manager: %s", esp_err_to_name(err));
        playlist_manager_deinit();
        free(found_animations_dir);
        playback_controller_deinit();
        display_renderer_deinit();
        return err;
    }

    // SD card path is no longer needed here - play_scheduler handles SD card channel internally
    free(found_animations_dir);
    found_animations_dir = NULL;

    // Boot-restore of the active playset is deferred to
    // animation_player_restore_boot_playset(), called from p3a_main.c after
    // pin_lists_init(). The boot logo (3250 ms total) covers the gap, so the
    // user never sees a blank screen between display init and the first
    // execute_playset() call.

    s_buffer_mutex = xSemaphoreCreateMutex();
    if (!s_buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create buffer mutex");
        content_cache_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_loader_sem = xSemaphoreCreateBinary();
    if (!s_loader_sem) {
        ESP_LOGE(TAG, "Failed to create loader semaphore");
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        content_cache_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_prefetch_done_sem = xSemaphoreCreateBinary();
    if (!s_prefetch_done_sem) {
        ESP_LOGE(TAG, "Failed to create prefetch done semaphore");
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        content_cache_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_ERR_NO_MEM;
    }

    memset(&s_front_buffer, 0, sizeof(s_front_buffer));
    memset(&s_back_buffer, 0, sizeof(s_back_buffer));

    // Initialize state-aware rendering EARLY so we can show UI messages
    // even before the first animation is loaded. This is critical for
    // showing "Downloading artwork..." messages on fresh boot.
    (void)p3a_render_init();
    display_renderer_set_frame_callback(animation_player_render_dispatch_cb, NULL);

    // The generic "Starting..." channel message is already set by p3a_state_init.
    // It will be replaced with the actual channel's display name when
    // animation_player_restore_boot_playset() runs (a few hundred ms later),
    // hidden behind the boot logo.


    // Mark front buffer as not ready - will be loaded by swap_to(0, 0) after loader starts
    s_front_buffer.ready = false;
    s_front_buffer.prefetch_pending = false;
    s_front_buffer.prefetch_in_progress = false;

    if (xTaskCreate(animation_loader_task,
                    "anim_loader",
                    8192,
                    NULL,
                    CONFIG_P3A_RENDER_TASK_PRIORITY - 1,
                    &s_loader_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create loader task");
        unload_animation_buffer(&s_front_buffer);
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        content_cache_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t animation_player_restore_boot_playset(void)
{
    /* Try the saved snapshot first. Any failure mode (missing file, version
       mismatch, corrupted CRC, execute failure) falls through to the Makapix
       Promoted default. The snapshot file is deleted by active_playset_load
       on corruption / version mismatch, so we don't accumulate stale state. */

    ps_playset_t *playset = psram_calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        ESP_LOGE(TAG, "Boot restore: OOM allocating playset; using legacy fallback");
        return play_scheduler_play_named_channel("promoted");
    }

    esp_err_t err = active_playset_load(playset);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Restoring active playset (name='%s', channels=%zu)",
                 playset->name, playset->channel_count);
        err = play_scheduler_execute_playset(playset, false);
        if (err == ESP_OK) {
            free(playset);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Restore execute failed: %s — falling back to channel_promoted",
                 esp_err_to_name(err));
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No active-playset snapshot; starting on channel_promoted");
    } else {
        ESP_LOGW(TAG, "active_playset_load failed: %s — falling back to channel_promoted",
                 esp_err_to_name(err));
    }

    /* Fallback: Makapix Promoted. Build it inline rather than depending on
       ps_create_channel_playset() so this path stays cheap and self-contained. */
    memset(playset, 0, sizeof(*playset));
    playset->channel_count = 1;
    playset->channels[0].type = PS_CHANNEL_TYPE_NAMED;
    strlcpy(playset->channels[0].name, "promoted", sizeof(playset->channels[0].name));
    playset->channels[0].weight = 1;
    err = play_scheduler_execute_playset(playset, false);
    free(playset);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Default Promoted execute failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t animation_player_load_asset(const char *filepath)
{
    (void)filepath;
    return ESP_ERR_NOT_SUPPORTED;
}

void animation_player_set_paused(bool paused)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_anim_paused = paused;
        xSemaphoreGive(s_buffer_mutex);
        ESP_LOGD(TAG, "Animation %s", paused ? "paused" : "resumed");
    }
}

void animation_player_toggle_pause(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_anim_paused = !s_anim_paused;
        bool paused = s_anim_paused;
        xSemaphoreGive(s_buffer_mutex);
        ESP_LOGD(TAG, "Animation %s", paused ? "paused" : "resumed");
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

// ============================================================================
// NEW SIMPLIFIED API (Phase 3 Refactor)
// ============================================================================

esp_err_t animation_player_request_swap(const swap_request_t *request)
{
    if (!request) {
        ESP_LOGE(TAG, "Invalid swap request: NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (display_renderer_is_ui_mode()) {
        ESP_LOGW(TAG, "Swap request ignored: UI mode active");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SD card is exported over USB");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (animation_player_is_sd_paused()) {
        ESP_LOGW(TAG, "Swap request ignored: SD access paused for OTA");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (sdio_bus_is_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SDIO bus locked by %s", 
                 sdio_bus_get_holder() ? sdio_bus_get_holder() : "unknown");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Swap request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        
        // Set up load override with validated swap request data
        memset(&s_load_override, 0, sizeof(s_load_override));
        s_load_override.valid = true;
        s_load_override.type = request->type;
        s_load_override.channel_type = request->channel_type;
        strlcpy(s_load_override.channel_spec_name, request->channel_spec_name, sizeof(s_load_override.channel_spec_name));
        strlcpy(s_load_override.channel_identifier, request->channel_identifier, sizeof(s_load_override.channel_identifier));
        s_load_override.post_id = request->post_id;
        s_load_override.post_source = request->post_source;
        s_load_override.fail_mode = request->fail_mode;
        strlcpy(s_load_override.filepath, request->filepath, sizeof(s_load_override.filepath));

        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);

        // A loud-fail (user-initiated) swap supersedes any silent-retry burst.
        if (request->fail_mode == SWAP_FAIL_LOUD) {
            animation_loader_reset_auto_retry_state();
        }
        
        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }
        
        ESP_LOGD(TAG, "Swap request accepted: %s (post_id=%d)", 
                 request->filepath, request->post_id);
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

void animation_player_display_message(const char *title, const char *body)
{
    if (!body) return;
    
    p3a_render_set_channel_message(
        title ? title : "Info",
        P3A_CHANNEL_MSG_ERROR,  // Use ERROR type for visibility
        -1,  // No timeout
        body
    );
    
    ESP_LOGI(TAG, "Displaying message: %s - %s", title, body);
}

// ============================================================================

esp_err_t animation_player_begin_sd_export(void)
{
    if (animation_player_is_sd_export_locked()) {
        return ESP_OK;
    }

    // Signal SD unavailable to pause any pending downloads
    makapix_channel_signal_sd_unavailable();

    animation_loader_wait_for_idle();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_export_active = true;
        s_swap_requested = false;
        s_back_buffer.prefetch_pending = false;
        s_back_buffer.prefetch_in_progress = false;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_export_active = true;
    }

    ESP_LOGD(TAG, "SD card exported to USB host");
    return ESP_OK;
}

esp_err_t animation_player_end_sd_export(void)
{
    if (!animation_player_is_sd_export_locked()) {
        return ESP_OK;
    }

    sd_refresh_request_t request = {
        .requester = xTaskGetCurrentTaskHandle(),
        .result = ESP_OK,
    };
    // Allocate PSRAM stack if not already allocated
    if (!s_sd_refresh_stack) {
        s_sd_refresh_stack = heap_caps_malloc(ANIMATION_SD_REFRESH_STACK * sizeof(StackType_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    TaskHandle_t task_handle = NULL;
    esp_err_t refresh_err = ESP_OK;
    if (s_sd_refresh_stack && !s_sd_refresh_task_running) {
        s_sd_refresh_task_running = true;
        task_handle = xTaskCreateStatic(animation_player_sd_refresh_task,
                                        "anim_sd_refresh",
                                        ANIMATION_SD_REFRESH_STACK,
                                        &request,
                                        CONFIG_P3A_RENDER_TASK_PRIORITY - 1,
                                        s_sd_refresh_stack,
                                        &s_sd_refresh_task_buffer);
    }

    if (!task_handle) {
        s_sd_refresh_task_running = false;
        if (xTaskCreate(animation_player_sd_refresh_task,
                        "anim_sd_refresh",
                        ANIMATION_SD_REFRESH_STACK,
                        &request,
                        CONFIG_P3A_RENDER_TASK_PRIORITY - 1,
                        NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create SD refresh task");
            refresh_err = ESP_ERR_NO_MEM;
        }
    }

    if (refresh_err == ESP_OK) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        refresh_err = request.result;
        if (refresh_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to refresh animation list after SD remount: %s",
                     esp_err_to_name(refresh_err));
        }
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_export_active = false;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_export_active = false;
    }

    // Signal SD available to resume any paused downloads
    makapix_channel_signal_sd_available();

    ESP_LOGD(TAG, "SD card returned to local control");
    return refresh_err;
}

bool animation_player_is_sd_export_locked(void)
{
    bool locked = s_sd_export_active;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        locked = s_sd_export_active;
        xSemaphoreGive(s_buffer_mutex);
    }
    return locked;
}

bool animation_player_is_loader_busy(void)
{
    bool busy = false;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        busy = s_loader_busy || s_swap_requested || s_back_buffer.prefetch_pending;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        busy = s_loader_busy || s_swap_requested;
    }
    return busy;
}

void animation_player_pause_sd_access(void)
{
    // First set the paused flag to prevent NEW operations from starting
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_access_paused = true;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_access_paused = true;
    }

    // Then wait for loader to become idle
    int wait_count = 0;
    while (animation_player_is_loader_busy() && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (wait_count >= 100) {
        ESP_LOGW(TAG, "Animation loader still busy after 10s");
    }

    ESP_LOGI(TAG, "SD card access paused for external operation");
}

void animation_player_resume_sd_access(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_access_paused = false;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_access_paused = false;
    }
    
    ESP_LOGD(TAG, "SD card access resumed");
}

bool animation_player_is_sd_paused(void)
{
    bool paused = s_sd_access_paused;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        paused = s_sd_access_paused;
        xSemaphoreGive(s_buffer_mutex);
    }
    return paused;
}

bool animation_player_is_animation_ready(void)
{
    bool ready = false;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ready = s_front_buffer.ready;
        xSemaphoreGive(s_buffer_mutex);
    }
    return ready;
}

void animation_player_invalidate(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_front_buffer.ready = false;
        xSemaphoreGive(s_buffer_mutex);
        ESP_LOGD(TAG, "Front buffer invalidated");
    }
}

esp_err_t animation_player_start(void)
{
    return display_renderer_start();
}

void animation_player_deinit(void)
{
#if CONFIG_P3A_PICO8_ENABLE
    pico8_render_deinit();
#endif
    s_sd_export_active = false;

    if (s_loader_task) {
        vTaskDelete(s_loader_task);
        s_loader_task = NULL;
    }

    unload_animation_buffer(&s_front_buffer);
    unload_animation_buffer(&s_back_buffer);

    if (s_loader_sem) {
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
    }
    if (s_buffer_mutex) {
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
    }

    free_sd_file_list();
    content_cache_deinit();
    playlist_manager_deinit();
    playback_controller_deinit();
    display_renderer_deinit();
    
    if (s_sd_mounted) {
        bsp_sdcard_unmount();
        s_sd_mounted = false;
    }
}

size_t animation_player_get_current_index(void)
{
    // Keep legacy semantics for /upload insertion:
    // return the current SD file list index, or SIZE_MAX if unknown/not playing.
    if (s_sd_file_list.count == 0) {
        return SIZE_MAX;
    }

    // current_index is maintained by the SD list logic (loader/refresh)
    return s_sd_file_list.current_index;
}

esp_err_t animation_player_swap_to_index(size_t index)
{
    if (animation_player_is_sd_export_locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    // play_scheduler uses virtual queue, direct index access is not supported
    ESP_LOGW(TAG, "Direct index swap not supported by play_scheduler");
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
}

// ============================================================================
// UI Mode Control
// ============================================================================

esp_err_t animation_player_enter_ui_mode(void)
{
    ESP_LOGD(TAG, "Entering UI mode");
    
    esp_err_t err = display_renderer_enter_ui_mode();
    if (err != ESP_OK) {
        return err;
    }
    
    // Unload animation buffers to free internal RAM for HTTP/SSL operations
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGD(TAG, "Unloading animation buffers to free memory for provisioning");
        unload_animation_buffer(&s_front_buffer);
        unload_animation_buffer(&s_back_buffer);
        s_swap_requested = false;
        xSemaphoreGive(s_buffer_mutex);
    }
    
    // Mark animation playback as stopped (no longer a resumption candidate)
    playback_controller_notify_animation_stopped();
    
    ESP_LOGD(TAG, "UI mode active");
    return ESP_OK;
}

void animation_player_exit_ui_mode(void)
{
    ESP_LOGD(TAG, "Exiting UI mode");
    
    // Trigger reload of current animation
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        xSemaphoreGive(s_loader_sem);
    }
    
    display_renderer_exit_ui_mode();
    ESP_LOGD(TAG, "Animation mode active");
}

bool animation_player_is_ui_mode(void)
{
    return display_renderer_is_ui_mode();
}

// ============================================================================
// Screen rotation
// ============================================================================

esp_err_t app_set_screen_rotation(screen_rotation_t rotation)
{
    esp_err_t err = render_engine_set_rotation(rotation);
    if (err == ESP_OK) {
        animation_player_render_on_rotation_changed(rotation);
        event_bus_emit_i32(P3A_EVENT_ROTATION_CHANGED, (int32_t)rotation);
    }
    return err;
}

screen_rotation_t app_get_screen_rotation(void)
{
    return display_renderer_get_rotation();
}
