#include "graphics_mode.h"

#include "player.h"
#include "ui_mode_switch.h"
#include "graphics_handoff.h"
#include "ui.h"

#include "p3a_hal/display.h"
#include "storage/fs.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_touch.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>

#define TAG "graphics_mode"

#define MAX_ANIMATIONS 64

// Playback policy toggles
static const bool s_include_gif_files = true;
static const bool s_include_webp_files = true;

typedef struct {
    char path[256];
    char name[64];
    int native_size;  // Detected or default native size
    anim_type_t type;
} animation_entry_t;

static animation_entry_t s_animations[MAX_ANIMATIONS];
static size_t s_animation_count;
static size_t s_current_index;

static bool scan_directory(const char *dir_path);
static esp_err_t populate_animation_list(void);
static void log_animation_list(void);
static esp_err_t switch_to_playback(size_t index);
static esp_err_t switch_to_lvgl(void);
static int detect_native_size(const char* path, anim_type_t type);
static void on_enter_player_mode(void);
static void on_enter_lvgl_mode(void);

void graphics_mode_init(void)
{
    ESP_LOGI(TAG, "=== Graphics mode init start ===");
    ESP_LOGI(TAG, "Animation scan policy: gif=%d webp=%d", s_include_gif_files, s_include_webp_files);

    // Initialize player system
    ESP_LOGI(TAG, "Initializing player system...");
    ESP_ERROR_CHECK(player_init());
    ESP_LOGI(TAG, "Player system initialized");

    // Initialize graphics handoff
    ESP_LOGI(TAG, "Initializing graphics handoff...");
    ESP_ERROR_CHECK(graphics_handoff_init());
    ESP_LOGI(TAG, "Graphics handoff initialized");

    // Initialize UI mode switch
    ESP_LOGI(TAG, "Initializing UI mode switch...");
    ESP_ERROR_CHECK(ui_mode_switch_init());
    ESP_LOGI(TAG, "UI mode switch initialized");

    // Set callbacks for mode switching
    ESP_LOGI(TAG, "Setting mode switch callbacks...");
    ui_mode_switch_set_callbacks(on_enter_player_mode, on_enter_lvgl_mode);

    // Register touch handle
    ESP_LOGI(TAG, "Registering touch handle...");
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        typedef struct {
            esp_lcd_touch_handle_t handle;
            float scale_x;
            float scale_y;
        } lvgl_port_touch_ctx_t;
        lvgl_port_touch_ctx_t *touch_ctx = (lvgl_port_touch_ctx_t *)lv_indev_get_driver_data(indev);
        if (touch_ctx) {
            ui_mode_switch_register_touch(touch_ctx->handle);
            ESP_LOGI(TAG, "Touch handle registered: %p", (void*)touch_ctx->handle);
        } else {
            ESP_LOGW(TAG, "Touch context is NULL");
        }
    } else {
        ESP_LOGW(TAG, "Input device is NULL");
    }

    // Start touch polling
    ESP_LOGI(TAG, "Starting touch polling...");
    esp_err_t ret = ui_mode_switch_start_touch_polling();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start touch polling: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Touch polling started");
    }

    // Populate animation list
    ESP_LOGI(TAG, "Populating animation list...");
    if (populate_animation_list() == ESP_OK && s_animation_count > 0) {
        ESP_LOGI(TAG, "Found %zu animations", s_animation_count);
        log_animation_list();
    } else {
        ESP_LOGW(TAG, "No animations found on SD card");
    }

    // Boot into player mode if animations exist, otherwise LVGL mode
    if (s_animation_count > 0) {
        ESP_LOGI(TAG, "Starting playback mode with first animation...");
        if (switch_to_playback(0) == ESP_OK) {
            ESP_LOGI(TAG, "Playback mode started with animation '%s'",
                     s_animations[s_current_index].name);
        } else {
            ESP_LOGW(TAG, "Playback start failed, falling back to LVGL mode");
            switch_to_lvgl();
        }
    } else {
        ESP_LOGI(TAG, "No animations found, starting LVGL mode...");
        switch_to_lvgl();
    }
    
    ESP_LOGI(TAG, "=== Graphics mode init complete ===");
}

void graphics_mode_handle_short_tap(void)
{
    if (ui_mode_switch_is_player_mode() && s_animation_count > 0) {
        size_t next = (s_current_index + 1) % s_animation_count;
        if (switch_to_playback(next) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch to next animation");
        }
    } else {
        ESP_LOGD(TAG, "Ignoring short tap in current mode");
    }
}

void graphics_mode_handle_long_tap(void)
{
    // Long-press handling is now done by ui_mode_switch
    // This function is kept for compatibility but may not be called
    if (ui_mode_switch_is_player_mode()) {
        switch_to_lvgl();
    } else {
        if (s_animation_count > 0) {
            switch_to_playback(s_current_index);
        }
    }
}

static void on_enter_player_mode(void)
{
    if (s_animation_count > 0) {
        switch_to_playback(s_current_index);
    }
}

static void on_enter_lvgl_mode(void)
{
    switch_to_lvgl();
}

static esp_err_t switch_to_playback(size_t index)
{
    if (s_animation_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (index >= s_animation_count) {
        index = 0;
    }

    const animation_entry_t* entry = &s_animations[index];
    ESP_LOGI(TAG, "Switching to playback animation #%zu: %s", index, entry->path);

    ui_hide();

    anim_desc_t desc = {
        .type = entry->type,
        .path = entry->path,
        .native_size_px = entry->native_size
    };

    if (!player_start(&desc)) {
        ESP_LOGE(TAG, "Failed to start player");
        return ESP_FAIL;
    }

    s_current_index = index;
    ui_mode_switch_enter_player_mode();
    return ESP_OK;
}

static esp_err_t switch_to_lvgl(void)
{
    ESP_LOGI(TAG, "Switching to LVGL mode");
    
    player_stop();
    ui_show();
    ui_mode_switch_enter_lvgl_mode();
    
    return ESP_OK;
}

static int detect_native_size(const char* path, anim_type_t type)
{
    // For now, default to 64x64. In a full implementation, this would
    // parse the file header to detect actual native size.
    // The user must specify the correct size in the descriptor.
    (void)path;
    (void)type;
    return 64;  // Default assumption
}

static esp_err_t populate_animation_list(void)
{
    s_animation_count = 0;

    if (!storage_fs_is_sd_present()) {
        ESP_LOGW(TAG, "SD card not present");
        return ESP_ERR_NOT_FOUND;
    }

    const char *sd_path = storage_fs_get_sd_path();
    if (!sd_path) {
        ESP_LOGW(TAG, "SD path not available");
        return ESP_ERR_INVALID_STATE;
    }

    char dir[256];
    snprintf(dir, sizeof(dir), "%s/animations", sd_path);
    if (!scan_directory(dir)) {
        ESP_LOGW(TAG, "Animation folder not found, scanning SD root");
        if (!scan_directory(sd_path)) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (s_animation_count > 0) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static bool has_animation_extension(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }

    bool is_gif = (len >= 4) && (strcasecmp(&name[len - 4], ".gif") == 0);
    bool is_webp = (len >= 5) && (strcasecmp(&name[len - 5], ".webp") == 0);

    if (is_gif && s_include_gif_files) {
        return true;
    }
    if (is_webp && s_include_webp_files) {
        return true;
    }
    return false;
}

static anim_type_t get_animation_type(const char* name)
{
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(&name[len - 4], ".gif") == 0) {
        return FILE_GIF;
    }
    if (len >= 5 && strcasecmp(&name[len - 5], ".webp") == 0) {
        return FILE_WEBP;
    }
    return FILE_GIF;  // Default
}

static bool scan_directory(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_animation_count < MAX_ANIMATIONS) {
        if (!has_animation_extension(entry->d_name)) {
            continue;
        }
        animation_entry_t *dst = &s_animations[s_animation_count];
        int ret = snprintf(dst->path, sizeof(dst->path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(dst->path)) {
            ESP_LOGW(TAG, "Path too long, skipping: %s/%s", dir_path, entry->d_name);
            continue;
        }
        strncpy(dst->name, entry->d_name, sizeof(dst->name) - 1);
        dst->name[sizeof(dst->name) - 1] = '\0';
        dst->type = get_animation_type(entry->d_name);
        dst->native_size = detect_native_size(dst->path, dst->type);
        s_animation_count++;
    }

    closedir(dir);
    return s_animation_count > 0;
}

static void log_animation_list(void)
{
    ESP_LOGI(TAG, "Found %zu animations:", s_animation_count);
    for (size_t i = 0; i < s_animation_count; ++i) {
        ESP_LOGI(TAG, "  %2zu: %s (%s, %dx%d)", 
                 i, s_animations[i].path,
                 s_animations[i].type == FILE_GIF ? "GIF" : "WebP",
                 s_animations[i].native_size, s_animations[i].native_size);
    }
}
