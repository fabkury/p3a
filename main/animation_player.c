#include "animation_player_priv.h"
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

// Animation player state
animation_buffer_t s_front_buffer = {0};
animation_buffer_t s_back_buffer = {0};
size_t s_next_asset_index = 0;
bool s_swap_requested = false;
bool s_loader_busy = false;
TaskHandle_t s_loader_task = NULL;
SemaphoreHandle_t s_loader_sem = NULL;
SemaphoreHandle_t s_buffer_mutex = NULL;

bool s_anim_paused = false;

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

    const char *preferred_dir = ANIMATIONS_PREFERRED_DIR;
    if (directory_has_animation_files(preferred_dir)) {
        *animations_dir_out = strdup(preferred_dir);
        if (!*animations_dir_out) {
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Using preferred animations directory: %s", preferred_dir);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Preferred directory empty or missing, searching SD card...");
    return find_animations_directory(BSP_SD_MOUNT_POINT, animations_dir_out);
}

static esp_err_t load_first_animation(void)
{
    const sdcard_post_t *post = NULL;
    esp_err_t err = channel_player_get_current_post(&post);
    if (err != ESP_OK || !post) {
        ESP_LOGE(TAG, "No current post available from channel player");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t load_err = load_animation_into_buffer(post->filepath, post->type, &s_front_buffer);
    if (load_err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded animation '%s' to start playback", post->name);
        
        // Update playback controller with metadata
        playback_controller_set_animation_metadata(post->filepath, true);
        
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to load animation '%s', trying next...", post->name);
    
    err = channel_player_advance();
    if (err == ESP_OK) {
        err = channel_player_get_current_post(&post);
        if (err == ESP_OK && post) {
            load_err = load_animation_into_buffer(post->filepath, post->type, &s_front_buffer);
            if (load_err == ESP_OK) {
                ESP_LOGI(TAG, "Loaded animation '%s' to start playback", post->name);
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
    channel_player_switch_to_sdcard_channel();

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
        ESP_LOGE(TAG, "No animation posts loaded");
        channel_player_deinit();
        download_manager_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return ESP_ERR_NOT_FOUND;
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

    err = load_first_animation();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to load initial animation: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        channel_player_deinit();
        download_manager_deinit();
        playlist_manager_deinit();
        playback_controller_deinit();
        display_renderer_deinit();
        return err;
    }

    esp_err_t prefetch_err = prefetch_first_frame(&s_front_buffer);
    if (prefetch_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to prefetch first frame during init: %s", esp_err_to_name(prefetch_err));
    }

    s_front_buffer.ready = true;
    s_front_buffer.prefetch_pending = false;

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

    // Set the frame callback for display renderer
    display_renderer_set_frame_callback(animation_player_render_frame_callback, NULL);

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
        ESP_LOGI(TAG, "Animation %s", paused ? "paused" : "resumed");
    }
}

void animation_player_toggle_pause(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_anim_paused = !s_anim_paused;
        bool paused = s_anim_paused;
        xSemaphoreGive(s_buffer_mutex);
        ESP_LOGI(TAG, "Animation %s", paused ? "paused" : "resumed");
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
    if (display_renderer_is_ui_mode()) {
        ESP_LOGW(TAG, "Swap request ignored: UI mode active");
        return;
    }

    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SD card is exported over USB");
        return;
    }

    if (animation_player_is_sd_paused()) {
        ESP_LOGW(TAG, "Swap request ignored: SD access paused for OTA");
        return;
    }

    // Check if SDIO bus is locked (e.g., by OTA check/download)
    // Discard swap to prevent concurrent SD card access during WiFi operations
    if (sdio_bus_is_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SDIO bus locked by %s", 
                 sdio_bus_get_holder() ? sdio_bus_get_holder() : "unknown");
        return;
    }

    if (ota_manager_is_checking()) {
        ESP_LOGW(TAG, "Swap request ignored: OTA check in progress (SDIO bus busy)");
        return;
    }

    if (channel_player_get_post_count() == 0) {
        ESP_LOGW(TAG, "No animations available to cycle");
        return;
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return;
        }
        xSemaphoreGive(s_buffer_mutex);
    }

    esp_err_t err = forward ? channel_player_advance() : channel_player_go_back();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to cycle animation: %s", esp_err_to_name(err));
        return;
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return;
        }

        s_next_asset_index = forward ? 1 : 0;
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);

        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }

        const sdcard_post_t *post = NULL;
        if (channel_player_get_current_post(&post) == ESP_OK && post) {
            ESP_LOGI(TAG, "Queued animation load to '%s'", post->name);
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
            ESP_LOGI(TAG, "Requested swap to current: '%s'", post->name);
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
    
    ESP_LOGI(TAG, "Executing swap_future: frame=%u, live=%d", 
             swap->start_frame, swap->is_live_mode_swap);
    
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
    
    // Check if swap already in progress
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "swap_future blocked: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        
        // For now, we trigger a regular swap since we need more infrastructure
        // to support loading arbitrary artworks (Phase C will add this)
        // TODO: Store swap->artwork info for loader to use
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        
        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }
        
        ESP_LOGI(TAG, "swap_future triggered loader (start_frame support in Phase B2)");
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t animation_player_begin_sd_export(void)
{
    if (animation_player_is_sd_export_locked()) {
        return ESP_OK;
    }

    animation_loader_wait_for_idle();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_sd_export_active = true;
        s_swap_requested = false;
        s_back_buffer.prefetch_pending = false;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_sd_export_active = true;
    }

    ESP_LOGI(TAG, "SD card exported to USB host");
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

    ESP_LOGI(TAG, "SD card returned to local control");
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
    
    ESP_LOGI(TAG, "SD card access resumed");
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
    ESP_LOGI(TAG, "Entering UI mode");
    
    esp_err_t err = display_renderer_enter_ui_mode();
    if (err != ESP_OK) {
        return err;
    }
    
    // Unload animation buffers to free internal RAM for HTTP/SSL operations
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Unloading animation buffers to free memory for provisioning");
        unload_animation_buffer(&s_front_buffer);
        unload_animation_buffer(&s_back_buffer);
        s_swap_requested = false;
        xSemaphoreGive(s_buffer_mutex);
    }
    
    // Clear metadata since we're not playing an animation
    playback_controller_clear_metadata();
    
    ESP_LOGI(TAG, "UI mode active");
    return ESP_OK;
}

void animation_player_exit_ui_mode(void)
{
    ESP_LOGI(TAG, "Exiting UI mode");
    
    // Trigger reload of current animation
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        xSemaphoreGive(s_loader_sem);
    }
    
    display_renderer_exit_ui_mode();
    ESP_LOGI(TAG, "Animation mode active");
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
    return display_renderer_set_rotation(rotation);
}

screen_rotation_t app_get_screen_rotation(void)
{
    return display_renderer_get_rotation();
}
