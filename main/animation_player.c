#include "animation_player_priv.h"
#include "sd_path.h"
#include "channel_player.h"
#include "sdcard_channel_impl.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "ugfx_ui.h"
#include "config_store.h"
#include "ota_manager.h"
#include "sdio_bus.h"
#include "pico8_stream.h"
#include "pico8_render.h"
#include "swap_future.h"
#include "p3a_state.h"
#include "makapix_channel_impl.h"
#include "makapix.h"
#include "p3a_render.h"
#include "display_renderer.h"
#include "makapix_channel_events.h"

// Animation player state
animation_buffer_t s_front_buffer = {0};
animation_buffer_t s_back_buffer = {0};
size_t s_next_asset_index = 0;
bool s_swap_requested = false;
bool s_loader_busy = false;
volatile bool s_cycle_pending = false;
volatile bool s_cycle_forward = true;
TaskHandle_t s_loader_task = NULL;
SemaphoreHandle_t s_loader_sem = NULL;
SemaphoreHandle_t s_buffer_mutex = NULL;

bool s_anim_paused = false;

// swap_future load override (one-shot)
animation_load_override_t s_load_override = {0};

app_lcd_sd_file_list_t s_sd_file_list = {0};
bool s_sd_mounted = false;
bool s_sd_export_active = false;
bool s_sd_access_paused = false;

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

static esp_err_t load_first_animation(void)
{
    const sdcard_post_t *post = NULL;
    esp_err_t err = channel_player_get_current_post(&post);
    if (err != ESP_OK || !post) {
        ESP_LOGE(TAG, "No current post available from channel player");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t load_err = load_animation_into_buffer(post->filepath, post->type, &s_front_buffer, 0, 0);
    if (load_err == ESP_OK) {
        ESP_LOGD(TAG, "Playing: %s", post->name);
        
        // Update playback controller with metadata
        playback_controller_set_animation_metadata(post->filepath, true);
        
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to load animation '%s', trying next...", post->name);
    
    err = channel_player_advance();
    if (err == ESP_OK) {
        err = channel_player_get_current_post(&post);
        if (err == ESP_OK && post) {
            load_err = load_animation_into_buffer(post->filepath, post->type, &s_front_buffer, 0, 0);
            if (load_err == ESP_OK) {
                ESP_LOGD(TAG, "Playing: %s", post->name);
                playback_controller_set_animation_metadata(post->filepath, true);
                return ESP_OK;
            }
        }
    }

    return load_err;
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

    err = download_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize download manager: %s", esp_err_to_name(err));
        playlist_manager_deinit();
        free(found_animations_dir);
        playback_controller_deinit();
        display_renderer_deinit();
        return err;
    }

    err = channel_player_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize channel player: %s", esp_err_to_name(err));
        download_manager_deinit();
        playlist_manager_deinit();
        free(found_animations_dir);
        playback_controller_deinit();
        display_renderer_deinit();
        return err;
    }

    // Initialize swap_future system for Live Mode
    err = swap_future_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize swap_future system: %s", esp_err_to_name(err));
        // Non-fatal - continue without swap_future support
    }

    // Create SD card channel handle using the discovered animations directory
    channel_handle_t sd_ch = sdcard_channel_create("SD Card", found_animations_dir);
    free(found_animations_dir);
    if (!sd_ch) {
        ESP_LOGE(TAG, "Failed to create SD card channel handle");
        channel_player_deinit();
        download_manager_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_ERR_NO_MEM;
    }

    channel_player_set_sdcard_channel_handle(sd_ch);
    // Restore last remembered channel BEFORE loading the first animation,
    // so boot doesn't briefly show SD card content then switch later.
    p3a_channel_info_t saved = {0};
    bool want_makapix = false;
    char mk_channel_id[128] = {0};
    char mk_channel_name[128] = {0};

    if (p3a_state_get_channel_info(&saved) == ESP_OK) {
        if (saved.type == P3A_CHANNEL_MAKAPIX_ALL) {
            snprintf(mk_channel_id, sizeof(mk_channel_id), "all");
            snprintf(mk_channel_name, sizeof(mk_channel_name), "Recent");
            want_makapix = true;
        } else if (saved.type == P3A_CHANNEL_MAKAPIX_PROMOTED) {
            snprintf(mk_channel_id, sizeof(mk_channel_id), "promoted");
            snprintf(mk_channel_name, sizeof(mk_channel_name), "Promoted");
            want_makapix = true;
        } else if (saved.type == P3A_CHANNEL_MAKAPIX_USER) {
            snprintf(mk_channel_id, sizeof(mk_channel_id), "user");
            snprintf(mk_channel_name, sizeof(mk_channel_name), "My Artworks");
            want_makapix = true;
        } else if (saved.type == P3A_CHANNEL_MAKAPIX_BY_USER && saved.identifier[0] != '\0') {
            snprintf(mk_channel_id, sizeof(mk_channel_id), "by_user_%s", saved.identifier);
            snprintf(mk_channel_name, sizeof(mk_channel_name), "@%s's Artworks", saved.identifier);
            want_makapix = true;
        } else if (saved.type == P3A_CHANNEL_MAKAPIX_HASHTAG && saved.identifier[0] != '\0') {
            snprintf(mk_channel_id, sizeof(mk_channel_id), "hashtag_%s", saved.identifier);
            snprintf(mk_channel_name, sizeof(mk_channel_name), "#%s", saved.identifier);
            want_makapix = true;
        }
    }

    if (want_makapix) {
        // Get dynamic paths
        char vault_path[128], channel_path[128];
        sd_path_get_vault(vault_path, sizeof(vault_path));
        sd_path_get_channel(channel_path, sizeof(channel_path));
        
        channel_handle_t mk_ch = makapix_channel_create(mk_channel_id, mk_channel_name, vault_path, channel_path);
        if (mk_ch) {
            // Give ownership to makapix module so future switches clean up correctly.
            makapix_adopt_channel_handle(mk_ch);
            channel_player_switch_to_makapix_channel(mk_ch);
        } else {
            ESP_LOGW(TAG, "Failed to create Makapix channel for boot restore, falling back to SD card");
            channel_player_switch_to_sdcard_channel();
        }
    } else {
        channel_player_switch_to_sdcard_channel();
    }

    err = channel_player_load_channel();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load channel: %s", esp_err_to_name(err));
        channel_player_deinit();
        download_manager_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return err;
    }

    if (channel_player_get_post_count() == 0) {
        ESP_LOGD(TAG, "Channel empty, will populate from server");
        // Don't fail - just continue with empty channel
        // The p3a_render system will show appropriate message (Connecting... / Loading...)
        // and playback will start once artworks are downloaded
    }

    s_buffer_mutex = xSemaphoreCreateMutex();
    if (!s_buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create buffer mutex");
        channel_player_deinit();
        download_manager_deinit();
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
        channel_player_deinit();
        download_manager_deinit();
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

    err = load_first_animation();
    if (err != ESP_OK) {
        // No animation available yet - this is OK for Makapix channels
        // where files may not be downloaded yet. Show loading message
        // and continue initialization. The loader task will pick up
        // animations as they become available.
        ESP_LOGW(TAG, "No initial animation available: %s (will wait for downloads)", esp_err_to_name(err));
        
        // Show a state-aware message even when no animation is available.
        // IMPORTANT: On some devices, UI text historically only appeared after one animation loaded.
        // The fix is to ensure we keep the render pipeline alive and always set an explicit channel message.
        channel_player_source_t src = channel_player_get_source_type();
        if (src == CHANNEL_PLAYER_SOURCE_SDCARD) {
            // Empty SD card boot: show "no artworks" and hint for provisioning
            p3a_render_set_channel_message("microSD card", P3A_CHANNEL_MSG_EMPTY, -1,
                                           "No artworks found on microSD card.\nLong-press to register.");
        } else {
            // Makapix boot: show connecting/loading
            p3a_render_set_channel_message("Makapix Club", P3A_CHANNEL_MSG_LOADING, -1,
                                           "Connecting to Makapix Club...");
        }
        
        // Mark front buffer as not ready - will be loaded by loader task
        s_front_buffer.ready = false;
        s_front_buffer.prefetch_pending = false;
        s_front_buffer.prefetch_in_progress = false;
    } else {
        esp_err_t prefetch_err = prefetch_first_frame(&s_front_buffer);
        if (prefetch_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to prefetch first frame during init: %s", esp_err_to_name(prefetch_err));
        }

        s_front_buffer.ready = true;
        s_front_buffer.prefetch_pending = false;
        s_front_buffer.prefetch_in_progress = false;
    }

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
        channel_player_deinit();
        download_manager_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_FAIL;
    }

    return ESP_OK;
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

void animation_player_cycle_animation(bool forward)
{
    // IMPORTANT: This function is called from the touch task. Keep it stack-light.
    // Do not call into channel/navigation code here; that can trigger Live Mode schedule builds
    // (deep stack) and overflow app_touch_task. Instead, defer all work to the loader task.
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            xSemaphoreGive(s_buffer_mutex);
            return;
        }

        // IMPORTANT: Do NOT call channel_player_advance/go_back in the touch task context.
        // Those paths can build Live Mode schedules and overflow the 4KB touch task stack.
        // Instead, defer channel navigation to the loader task (which has a larger stack).
        s_cycle_pending = true;
        s_cycle_forward = forward;
        s_next_asset_index = forward ? 1 : 0;
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);

        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }
    }
}

esp_err_t animation_player_request_swap_current(void)
{
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

    // Check if SDIO bus is locked (e.g., by OTA check/download)
    // Discard swap to prevent concurrent SD card access during WiFi operations
    if (sdio_bus_is_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SDIO bus locked by %s", 
                 sdio_bus_get_holder() ? sdio_bus_get_holder() : "unknown");
        return ESP_ERR_INVALID_STATE;
    }

    if (channel_player_get_post_count() == 0) {
        ESP_LOGW(TAG, "No animations available to swap");
        return ESP_ERR_NOT_FOUND;
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Swap request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        // Don't advance - just request swap to current item
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);

        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }

        const sdcard_post_t *post = NULL;
        if (channel_player_get_current_post(&post) == ESP_OK && post) {
            ESP_LOGD(TAG, "Requested swap to current: '%s'", post->name);
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t swap_future_execute(const swap_future_t *swap)
{
    if (!swap || !swap->valid) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Executing swap_future: frame=%u, live=%d", 
             swap->start_frame, swap->is_live_mode_swap);

    if (swap->artwork.filepath[0] == '\0') {
        ESP_LOGW(TAG, "swap_future invalid: artwork filepath missing");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if OTA is in progress
    if (ota_manager_is_checking()) {
        ESP_LOGW(TAG, "swap_future blocked: OTA check in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if SD access is paused
    if (animation_player_is_sd_paused()) {
        ESP_LOGW(TAG, "swap_future blocked: SD access paused");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if swap already in progress and install one-shot loader override.
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "swap_future blocked: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        memset(&s_load_override, 0, sizeof(s_load_override));
        s_load_override.valid = true;
        strlcpy(s_load_override.filepath, swap->artwork.filepath, sizeof(s_load_override.filepath));
        s_load_override.type = swap->artwork.type;
        s_load_override.start_frame = swap->start_frame;
        s_load_override.start_time_ms = swap->start_time_ms;
        s_load_override.is_live_mode_swap = swap->is_live_mode_swap;
        s_load_override.live_index = swap->live_index;

        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        
        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }
        
        ESP_LOGD(TAG, "swap_future triggered loader: %s (type=%d start_frame=%u start_time_ms=%llu)",
                 s_load_override.filepath, (int)s_load_override.type,
                 (unsigned)s_load_override.start_frame, (unsigned long long)s_load_override.start_time_ms);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

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
    esp_err_t refresh_err = ESP_OK;
    if (xTaskCreate(animation_player_sd_refresh_task,
                    "anim_sd_refresh",
                    ANIMATION_SD_REFRESH_STACK,
                    &request,
                    CONFIG_P3A_RENDER_TASK_PRIORITY - 1,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SD refresh task");
        refresh_err = ESP_ERR_NO_MEM;
    } else {
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
    int wait_count = 0;
    while (animation_player_is_loader_busy() && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
    
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_access_paused = true;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_access_paused = true;
    }
    
    ESP_LOGD(TAG, "SD card access paused for external operation");
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

    swap_future_deinit();

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
    channel_player_deinit();
    download_manager_deinit();
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
    return channel_player_get_current_position();
}

esp_err_t animation_player_swap_to_index(size_t index)
{
    if (animation_player_is_sd_export_locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t post_count = channel_player_get_post_count();
    if (post_count == 0) {
        ESP_LOGW(TAG, "No animations available to swap");
        return ESP_ERR_NOT_FOUND;
    }

    if (index >= post_count) {
        ESP_LOGE(TAG, "Invalid index: %zu (max: %zu)", index, post_count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "Direct index swap not yet supported with channel abstraction");
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
    
    // Clear metadata since we're not playing an animation
    playback_controller_clear_metadata();
    
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
// PICO-8 compatibility wrapper
// ============================================================================

esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len)
{
#if CONFIG_P3A_PICO8_ENABLE
    return pico8_render_submit_frame(palette_rgb, palette_len, pixel_data, pixel_len);
#else
    (void)palette_rgb;
    (void)palette_len;
    (void)pixel_data;
    (void)pixel_len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

// ============================================================================
// Screen rotation
// ============================================================================

esp_err_t app_set_screen_rotation(screen_rotation_t rotation)
{
    esp_err_t err = display_renderer_set_rotation(rotation);
    if (err == ESP_OK) {
        animation_player_render_on_rotation_changed(rotation);
    }
    return err;
}

screen_rotation_t app_get_screen_rotation(void)
{
    return display_renderer_get_rotation();
}
