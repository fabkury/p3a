#include "animation_player_priv.h"
#include "channel_player.h"
#include "sdcard_channel.h"
#include "ugfx_ui.h"
#include "config_store.h"

esp_lcd_panel_handle_t s_display_handle = NULL;
uint8_t **s_lcd_buffers = NULL;
uint8_t s_buffer_count = 0;
size_t s_frame_buffer_bytes = 0;
size_t s_frame_row_stride_bytes = 0;

SemaphoreHandle_t s_vsync_sem = NULL;
TaskHandle_t s_anim_task = NULL;

animation_buffer_t s_front_buffer = {0};
animation_buffer_t s_back_buffer = {0};
size_t s_next_asset_index = 0;
bool s_swap_requested = false;
bool s_loader_busy = false;
TaskHandle_t s_loader_task = NULL;
SemaphoreHandle_t s_loader_sem = NULL;
SemaphoreHandle_t s_buffer_mutex = NULL;

bool s_anim_paused = false;
volatile render_mode_t s_render_mode_request = RENDER_MODE_ANIMATION;
volatile render_mode_t s_render_mode_active = RENDER_MODE_ANIMATION;

TaskHandle_t s_upscale_worker_top = NULL;
TaskHandle_t s_upscale_worker_bottom = NULL;
TaskHandle_t s_upscale_main_task = NULL;
const uint8_t *s_upscale_src_buffer = NULL;
uint8_t *s_upscale_dst_buffer = NULL;
const uint16_t *s_upscale_lookup_x = NULL;
const uint16_t *s_upscale_lookup_y = NULL;
int s_upscale_src_w = 0;
int s_upscale_src_h = 0;
screen_rotation_t s_upscale_rotation = ROTATION_0;
int s_upscale_row_start_top = 0;
int s_upscale_row_end_top = 0;
int s_upscale_row_start_bottom = 0;
int s_upscale_row_end_bottom = 0;
volatile bool s_upscale_worker_top_done = false;
volatile bool s_upscale_worker_bottom_done = false;

uint8_t s_render_buffer_index = 0;
uint8_t s_last_display_buffer = 0;

int64_t s_last_frame_present_us = 0;
int64_t s_last_duration_update_us = 0;
int s_latest_frame_duration_ms = 0;
char s_frame_duration_text[11] = "";
int64_t s_frame_processing_start_us = 0;
uint32_t s_target_frame_delay_ms = 0;

app_lcd_sd_file_list_t s_sd_file_list = {0};
bool s_sd_mounted = false;
bool s_sd_export_active = false;

// Screen rotation state
screen_rotation_t g_screen_rotation = ROTATION_0;
volatile bool g_rotation_in_progress = false;

typedef struct {
    TaskHandle_t requester;
    esp_err_t result;
} sd_refresh_request_t;

static void animation_player_wait_for_render_mode(render_mode_t target_mode)
{
    const TickType_t check_delay = pdMS_TO_TICKS(5);
    const TickType_t timeout = pdMS_TO_TICKS(500);
    TickType_t waited = 0;

    while (s_render_mode_active != target_mode) {
        vTaskDelay(check_delay);
        waited += check_delay;
        if (waited >= timeout) {
            ESP_LOGW(TAG, "Timed out waiting for render mode %d (active=%d)",
                     target_mode, s_render_mode_active);
            break;
        }
    }
}

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

static esp_err_t prepare_vsync(void)
{
    if (s_buffer_count > 1) {
        if (s_vsync_sem == NULL) {
            s_vsync_sem = xSemaphoreCreateBinary();
        }
        if (s_vsync_sem == NULL) {
            ESP_LOGE(TAG, "Failed to allocate VSYNC semaphore");
            return ESP_ERR_NO_MEM;
        }
        (void)xSemaphoreTake(s_vsync_sem, 0);
        xSemaphoreGive(s_vsync_sem);

        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_refresh_done = lcd_panel_refresh_done_cb,
        };
        return esp_lcd_dpi_panel_register_event_callbacks(s_display_handle, &cbs, s_vsync_sem);
    }

    if (s_vsync_sem) {
        vSemaphoreDelete(s_vsync_sem);
        s_vsync_sem = NULL;
        ESP_LOGW(TAG, "Single LCD frame buffer in use; tearing may occur");
    }
    return ESP_OK;
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
    // Get current post from channel player
    const sdcard_post_t *post = NULL;
    esp_err_t err = channel_player_get_current_post(&post);
    if (err != ESP_OK || !post) {
        ESP_LOGE(TAG, "No current post available from channel player");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t load_err = load_animation_into_buffer(post->filepath, post->type, &s_front_buffer);
    if (load_err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded animation '%s' to start playback", post->name);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to load animation '%s', trying next...", post->name);
    
    // Try advancing and loading next post
    err = channel_player_advance();
    if (err == ESP_OK) {
        err = channel_player_get_current_post(&post);
        if (err == ESP_OK && post) {
            load_err = load_animation_into_buffer(post->filepath, post->type, &s_front_buffer);
            if (load_err == ESP_OK) {
                ESP_LOGI(TAG, "Loaded animation '%s' to start playback", post->name);
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

    s_display_handle = display_handle;
    s_lcd_buffers = lcd_buffers;
    s_buffer_count = buffer_count;
    s_frame_buffer_bytes = buffer_bytes;
    s_frame_row_stride_bytes = row_stride_bytes;

    esp_err_t err = prepare_vsync();
    if (err != ESP_OK) {
        return err;
    }

    // Initialize channel system
    err = sdcard_channel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SD card channel: %s", esp_err_to_name(err));
        return err;
    }

    err = channel_player_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize channel player: %s", esp_err_to_name(err));
        sdcard_channel_deinit();
        return err;
    }

    char *found_animations_dir = NULL;
    err = mount_sd_and_discover(&found_animations_dir);
    if (err != ESP_OK || !found_animations_dir) {
        ESP_LOGE(TAG, "Failed to find animations directory: %s", esp_err_to_name(err));
        if (found_animations_dir) {
            free(found_animations_dir);
        }
        channel_player_deinit();
        sdcard_channel_deinit();
        return (err == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    err = sdcard_channel_refresh(found_animations_dir);
    free(found_animations_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh channel: %s", esp_err_to_name(err));
        channel_player_deinit();
        sdcard_channel_deinit();
        return err;
    }

    err = channel_player_load_channel();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load channel: %s", esp_err_to_name(err));
        channel_player_deinit();
        sdcard_channel_deinit();
        return err;
    }

    if (channel_player_get_post_count() == 0) {
        ESP_LOGE(TAG, "No animation posts loaded");
        channel_player_deinit();
        sdcard_channel_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    s_buffer_mutex = xSemaphoreCreateMutex();
    if (!s_buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create buffer mutex");
        return ESP_ERR_NO_MEM;
    }

    s_loader_sem = xSemaphoreCreateBinary();
    if (!s_loader_sem) {
        ESP_LOGE(TAG, "Failed to create loader semaphore");
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
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
        return err;
    }

    if (s_upscale_worker_top == NULL) {
        if (xTaskCreatePinnedToCore(upscale_worker_top_task,
                                    "upscale_top",
                                    2048,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &s_upscale_worker_top,
                                    0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create top upscale worker task");
            unload_animation_buffer(&s_front_buffer);
            vSemaphoreDelete(s_loader_sem);
            s_loader_sem = NULL;
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            return ESP_FAIL;
        }
    }

    if (s_upscale_worker_bottom == NULL) {
        if (xTaskCreatePinnedToCore(upscale_worker_bottom_task,
                                    "upscale_bottom",
                                    2048,
                                    NULL,
                                    CONFIG_P3A_RENDER_TASK_PRIORITY,
                                    &s_upscale_worker_bottom,
                                    1) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create bottom upscale worker task");
            unload_animation_buffer(&s_front_buffer);
            vSemaphoreDelete(s_loader_sem);
            s_loader_sem = NULL;
            vSemaphoreDelete(s_buffer_mutex);
            s_buffer_mutex = NULL;
            return ESP_FAIL;
        }
    }

    esp_err_t prefetch_err = prefetch_first_frame(&s_front_buffer);
    if (prefetch_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to prefetch first frame during init: %s", esp_err_to_name(prefetch_err));
    }

    s_front_buffer.ready = true;
    s_front_buffer.prefetch_pending = false;

    if (xTaskCreate(animation_loader_task,
                    "anim_loader",
                    4096,
                    NULL,
                    CONFIG_P3A_RENDER_TASK_PRIORITY - 1,
                    &s_loader_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create loader task");
        unload_animation_buffer(&s_front_buffer);
        vSemaphoreDelete(s_loader_sem);
        s_loader_sem = NULL;
        vSemaphoreDelete(s_buffer_mutex);
        s_buffer_mutex = NULL;
        return ESP_FAIL;
    }

    // Load and apply saved rotation
    screen_rotation_t saved_rotation = config_store_get_rotation();
    if (saved_rotation != ROTATION_0) {
        ESP_LOGI(TAG, "Restoring saved rotation: %d degrees", saved_rotation);
        g_screen_rotation = saved_rotation;
        // Apply to µGFX (animation lookup tables will use it automatically on first load)
        ugfx_ui_set_rotation(saved_rotation);
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
    if (animation_player_is_ui_mode()) {
        ESP_LOGW(TAG, "Swap request ignored: UI mode active");
        return;
    }

    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SD card is exported over USB");
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

    // Advance or go back in channel player (outside mutex to avoid deadlock)
    esp_err_t err = forward ? channel_player_advance() : channel_player_go_back();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to cycle animation: %s", esp_err_to_name(err));
        return;
    }

    // Now set the swap request flag
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // Double-check conditions after advance (in case something changed)
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return;
        }

        // Set flag for loader task (0 = backward, non-zero = forward)
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

esp_err_t animation_player_start(void)
{
    if (s_anim_task == NULL) {
        if (xTaskCreate(lcd_animation_task,
                        "lcd_anim",
                        4096,
                        NULL,
                        CONFIG_P3A_RENDER_TASK_PRIORITY,
                        &s_anim_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start LCD animation task");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void animation_player_deinit(void)
{
#if CONFIG_P3A_PICO8_ENABLE
    release_pico8_resources();
#endif
    s_sd_export_active = false;

    if (s_anim_task) {
        vTaskDelete(s_anim_task);
        s_anim_task = NULL;
    }

    if (s_loader_task) {
        vTaskDelete(s_loader_task);
        s_loader_task = NULL;
    }

    if (s_upscale_worker_top) {
        vTaskDelete(s_upscale_worker_top);
        s_upscale_worker_top = NULL;
    }
    if (s_upscale_worker_bottom) {
        vTaskDelete(s_upscale_worker_bottom);
        s_upscale_worker_bottom = NULL;
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
    if (s_vsync_sem) {
        vSemaphoreDelete(s_vsync_sem);
        s_vsync_sem = NULL;
    }

    free_sd_file_list();
    channel_player_deinit();
    sdcard_channel_deinit();
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

    // Note: Direct index access not directly supported by channel_player
    // This would require additional API. For now, return not supported.
    ESP_LOGW(TAG, "Direct index swap not yet supported with channel abstraction");
    return ESP_ERR_NOT_SUPPORTED;
}

// ============================================================================
// UI Mode Control - SIMPLE VERSION
// ============================================================================

esp_err_t animation_player_enter_ui_mode(void)
{
    ESP_LOGI(TAG, "Entering UI mode");
    s_render_mode_request = RENDER_MODE_UI;
    animation_player_wait_for_render_mode(RENDER_MODE_UI);
    
    // Unload animation buffers to free internal RAM for HTTP/SSL operations
    // This is critical because mbedTLS needs internal RAM for SSL buffers
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Unloading animation buffers to free memory for provisioning");
        unload_animation_buffer(&s_front_buffer);
        unload_animation_buffer(&s_back_buffer);
        s_swap_requested = false;
        xSemaphoreGive(s_buffer_mutex);
    }
    
    ESP_LOGI(TAG, "UI mode active");
    return ESP_OK;
}

void animation_player_exit_ui_mode(void)
{
    ESP_LOGI(TAG, "Exiting UI mode");
    
    // Trigger reload of current animation (buffers were unloaded when entering UI mode)
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // Signal loader to reload current animation into back buffer
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        xSemaphoreGive(s_loader_sem);  // Wake up loader task
    }
    
    s_render_mode_request = RENDER_MODE_ANIMATION;
    animation_player_wait_for_render_mode(RENDER_MODE_ANIMATION);
    ESP_LOGI(TAG, "Animation mode active");
}

bool animation_player_is_ui_mode(void)
{
    return (s_render_mode_active == RENDER_MODE_UI);
}

// Screen rotation implementation
esp_err_t app_set_screen_rotation(screen_rotation_t rotation)
{
    // Validate rotation angle
    if (rotation != ROTATION_0 && rotation != ROTATION_90 && 
        rotation != ROTATION_180 && rotation != ROTATION_270) {
        ESP_LOGE(TAG, "Invalid rotation angle: %d", rotation);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if rotation operation already in progress
    if (g_rotation_in_progress) {
        ESP_LOGW(TAG, "Rotation operation already in progress, rejecting new request");
        return ESP_ERR_INVALID_STATE;
    }
    
    // If same as current rotation, nothing to do
    if (rotation == g_screen_rotation) {
        ESP_LOGI(TAG, "Already at rotation %d degrees", rotation);
        return ESP_OK;
    }
    
    // Set rotation in progress flag
    g_rotation_in_progress = true;
    
    // Update global state
    screen_rotation_t old_rotation = g_screen_rotation;
    g_screen_rotation = rotation;
    
    ESP_LOGI(TAG, "Setting screen rotation from %d to %d degrees", old_rotation, rotation);
    
    // Apply to µGFX UI immediately
    esp_err_t err = ugfx_ui_set_rotation(rotation);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set µGFX rotation: %s", esp_err_to_name(err));
        // Continue anyway - UI rotation is non-critical
    }
    
    // Trigger animation reload with new rotation
    // The loader task will regenerate lookup tables
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        xSemaphoreGive(s_loader_sem);
    }
    
    // Store in config for persistence
    config_store_set_rotation(rotation);
    
    // Clear rotation in progress flag after a small delay to allow operations to start
    // We don't need to wait for completion, just ensure the request is processed
    vTaskDelay(pdMS_TO_TICKS(50));
    g_rotation_in_progress = false;
    
    ESP_LOGI(TAG, "Screen rotation set to %d degrees", rotation);
    return ESP_OK;
}

screen_rotation_t app_get_screen_rotation(void)
{
    return g_screen_rotation;
}
