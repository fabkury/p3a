#include "animation_player_priv.h"

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

TaskHandle_t s_upscale_worker_top = NULL;
TaskHandle_t s_upscale_worker_bottom = NULL;
TaskHandle_t s_upscale_main_task = NULL;
const uint8_t *s_upscale_src_buffer = NULL;
uint8_t *s_upscale_dst_buffer = NULL;
const uint16_t *s_upscale_lookup_x = NULL;
const uint16_t *s_upscale_lookup_y = NULL;
int s_upscale_src_w = 0;
int s_upscale_src_h = 0;
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
uint32_t s_target_frame_delay_ms = 16;

app_lcd_sd_file_list_t s_sd_file_list = {0};
bool s_sd_mounted = false;
bool s_sd_export_active = false;

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
    size_t start_index = 0;
    if (s_sd_file_list.health_flags) {
        bool found_healthy = false;
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            if (s_sd_file_list.health_flags[i]) {
                start_index = i;
                found_healthy = true;
                break;
            }
        }
        if (!found_healthy) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    esp_err_t load_err = load_animation_into_buffer(start_index, &s_front_buffer);
    if (load_err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded animation at index %zu to start playback", start_index);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to load animation at index %zu, searching for alternatives...", start_index);
    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        if (s_sd_file_list.health_flags && !s_sd_file_list.health_flags[i]) {
            continue;
        }
        if (i == start_index) {
            continue;
        }
        load_err = load_animation_into_buffer(i, &s_front_buffer);
        if (load_err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded animation at index %zu to start playback", i);
            return ESP_OK;
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

    char *found_animations_dir = NULL;
    err = mount_sd_and_discover(&found_animations_dir);
    if (err != ESP_OK || !found_animations_dir) {
        ESP_LOGE(TAG, "Failed to find animations directory: %s", esp_err_to_name(err));
        if (found_animations_dir) {
            free(found_animations_dir);
        }
        return (err == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    err = enumerate_animation_files(found_animations_dir);
    free(found_animations_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enumerate animation files: %s", esp_err_to_name(err));
        return err;
    }

    if (s_sd_file_list.count == 0) {
        ESP_LOGE(TAG, "No animation files found");
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
    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Swap request ignored: SD card is exported over USB");
        return;
    }

    if (s_sd_file_list.count == 0) {
        ESP_LOGW(TAG, "No animations available to cycle");
        return;
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return;
        }

        size_t current_index = s_front_buffer.ready ? s_front_buffer.asset_index : 0;
        size_t target_index = forward ? get_next_asset_index(current_index) : get_previous_asset_index(current_index);

        if (target_index == current_index && s_sd_file_list.health_flags) {
            bool any_healthy = false;
            for (size_t i = 0; i < s_sd_file_list.count; i++) {
                if (s_sd_file_list.health_flags[i]) {
                    any_healthy = true;
                    break;
                }
            }
            if (!any_healthy) {
                ESP_LOGW(TAG, "No healthy animation files available. Cannot cycle animation.");
                xSemaphoreGive(s_buffer_mutex);
                return;
            }
        }

        s_next_asset_index = target_index;
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);

        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }

        ESP_LOGI(TAG, "Queued animation load to '%s' (index %zu)",
                 s_sd_file_list.filenames[target_index], target_index);
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
    release_pico8_resources();
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
    if (s_sd_mounted) {
        bsp_sdcard_unmount();
        s_sd_mounted = false;
    }
}

size_t animation_player_get_current_index(void)
{
    size_t current_index = SIZE_MAX;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_front_buffer.ready) {
            current_index = s_front_buffer.asset_index;
        }
        xSemaphoreGive(s_buffer_mutex);
    }
    return current_index;
}

esp_err_t animation_player_swap_to_index(size_t index)
{
    if (animation_player_is_sd_export_locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sd_file_list.count == 0) {
        ESP_LOGW(TAG, "No animations available to swap");
        return ESP_ERR_NOT_FOUND;
    }

    if (index >= s_sd_file_list.count) {
        ESP_LOGE(TAG, "Invalid index: %zu (max: %zu)", index, s_sd_file_list.count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sd_file_list.health_flags && !s_sd_file_list.health_flags[index]) {
        ESP_LOGW(TAG, "File at index %zu is marked as unhealthy", index);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_swap_requested || s_loader_busy || s_back_buffer.prefetch_pending) {
            ESP_LOGW(TAG, "Animation change request ignored: swap already in progress");
            xSemaphoreGive(s_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        s_next_asset_index = index;
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);

        if (s_loader_sem) {
            xSemaphoreGive(s_loader_sem);
        }

        ESP_LOGI(TAG, "Queued animation load to '%s' (index %zu)",
                 s_sd_file_list.filenames[index], index);
        return ESP_OK;
    }

    return ESP_FAIL;
}

