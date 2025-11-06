#include "ui_mode_switch.h"

#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ui_mode_switch";

#define LONG_PRESS_THRESHOLD_US 600000  // 600ms
#define DEBOUNCE_US 50000               // 50ms
#define TOUCH_POLL_DELAY_MS 10          // ~100 Hz polling

typedef enum {
    UI_MODE_PLAYER = 0,
    UI_MODE_LVGL = 1,
} ui_mode_t;

static ui_mode_t s_current_mode = UI_MODE_PLAYER;  // Boot into player mode
static SemaphoreHandle_t s_mode_mutex = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static TaskHandle_t s_touch_task = NULL;
static bool s_initialized = false;

// Callback function pointers (set by user)
static void (*s_on_enter_player_mode)(void) = NULL;
static void (*s_on_enter_lvgl_mode)(void) = NULL;

static void touch_poll_task(void* arg);

esp_err_t ui_mode_switch_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mode_mutex = xSemaphoreCreateMutex();
    if (!s_mode_mutex) {
        ESP_LOGE(TAG, "Failed to create mode mutex");
        return ESP_ERR_NO_MEM;
    }

    s_current_mode = UI_MODE_PLAYER;  // Default to player mode
    s_touch_handle = NULL;
    s_touch_task = NULL;
    s_on_enter_player_mode = NULL;
    s_on_enter_lvgl_mode = NULL;
    s_initialized = true;

    ESP_LOGI(TAG, "UI mode switch initialized (default: PLAYER mode)");
    return ESP_OK;
}

void ui_mode_switch_register_touch(void* touch_handle)
{
    s_touch_handle = (esp_lcd_touch_handle_t)touch_handle;
    ESP_LOGI(TAG, "Touch handle registered: %p", (void*)touch_handle);
}

esp_err_t ui_mode_switch_start_touch_polling(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Mode switch not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_touch_task) {
        ESP_LOGW(TAG, "Touch polling task already running");
        return ESP_OK;
    }

    if (!s_touch_handle) {
        ESP_LOGW(TAG, "No touch handle registered, skipping touch polling");
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreatePinnedToCore(
            touch_poll_task,
            "ui_mode_touch",
            4096,
            NULL,
            3,  // Priority 3
            &s_touch_task,
            1) != pdPASS) {  // Core 1
        ESP_LOGE(TAG, "Failed to create touch polling task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Touch polling task started");
    return ESP_OK;
}

void ui_mode_switch_enter_player_mode(void)
{
    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_mode_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_current_mode == UI_MODE_PLAYER) {
        xSemaphoreGive(s_mode_mutex);
        return;
    }

    ESP_LOGI(TAG, "Switching to PLAYER mode");
    s_current_mode = UI_MODE_PLAYER;

    xSemaphoreGive(s_mode_mutex);

    if (s_on_enter_player_mode) {
        s_on_enter_player_mode();
    }
}

void ui_mode_switch_enter_lvgl_mode(void)
{
    if (!s_initialized) {
        return;
    }

    if (xSemaphoreTake(s_mode_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_current_mode == UI_MODE_LVGL) {
        xSemaphoreGive(s_mode_mutex);
        return;
    }

    ESP_LOGI(TAG, "Switching to LVGL mode");
    s_current_mode = UI_MODE_LVGL;

    xSemaphoreGive(s_mode_mutex);

    if (s_on_enter_lvgl_mode) {
        s_on_enter_lvgl_mode();
    }
}

bool ui_mode_switch_is_player_mode(void)
{
    if (!s_initialized) {
        return false;
    }
    return s_current_mode == UI_MODE_PLAYER;
}

// Set callback functions (internal use, called by player/graphics_mode)
void ui_mode_switch_set_callbacks(void (*on_player)(void), void (*on_lvgl)(void))
{
    s_on_enter_player_mode = on_player;
    s_on_enter_lvgl_mode = on_lvgl;
}

static void touch_poll_task(void* arg)
{
    (void)arg;
    if (!s_touch_handle) {
        ESP_LOGE(TAG, "Touch handle unavailable; terminating touch task");
        vTaskDelete(NULL);
        return;
    }

    bool was_pressed = false;
    int64_t press_start_us = 0;
    uint16_t touch_x[1];
    uint16_t touch_y[1];
    uint8_t point_count = 0;

    ESP_LOGI(TAG, "Touch polling task started");

    while (1) {
        if (esp_lcd_touch_read_data(s_touch_handle) == ESP_OK) {
            bool pressed = esp_lcd_touch_get_coordinates(s_touch_handle,
                                                          touch_x,
                                                          touch_y,
                                                          NULL,
                                                          &point_count,
                                                          1);

            if (pressed && point_count > 0 && !was_pressed) {
                // Touch down
                was_pressed = true;
                press_start_us = esp_timer_get_time();
            } else if (!pressed && was_pressed) {
                // Touch up
                was_pressed = false;
                int64_t duration_us = esp_timer_get_time() - press_start_us;
                
                // Check for long press (≥600ms)
                if (duration_us >= LONG_PRESS_THRESHOLD_US) {
                    // Debounce: ensure we're not in a rapid press sequence
                    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_US / 1000));
                    
                    // Toggle mode on release
                    if (ui_mode_switch_is_player_mode()) {
                        ui_mode_switch_enter_lvgl_mode();
                    } else {
                        ui_mode_switch_enter_player_mode();
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_DELAY_MS));
    }
}

