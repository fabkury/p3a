/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_lcd.h"
#include "app_touch.h"
#include "app_usb.h"
#include "app_wifi.h"
#include "http_api.h"
#include "fs_init.h"
#include "makapix.h"
#include "sntp_sync.h"
#include "ugfx_ui.h"
#include "freertos/task.h"

static const char *TAG = "p3a";

// Debug provisioning mode - toggle every 5 seconds
#define DEBUG_PROVISIONING_ENABLED 0
#define DEBUG_PROVISIONING_TOGGLE_MS 5000

#define AUTO_SWAP_INTERVAL_SECONDS CONFIG_P3A_AUTO_SWAP_INTERVAL_SECONDS

static TaskHandle_t s_auto_swap_task_handle = NULL;

static void auto_swap_task(void *arg)
{
    (void)arg;
    const TickType_t delay_ticks = pdMS_TO_TICKS(AUTO_SWAP_INTERVAL_SECONDS * 1000);
    
    ESP_LOGI(TAG, "Auto-swap task started: will cycle forward every %d seconds", AUTO_SWAP_INTERVAL_SECONDS);
    
    // Wait a bit for system to initialize before first swap
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while (true) {
        // Wait for interval or notification (which resets the timer)
        uint32_t notified = ulTaskNotifyTake(pdTRUE, delay_ticks);
        if (notified > 0) {
            ESP_LOGD(TAG, "Auto-swap timer reset by user interaction");
            continue;  // Timer was reset, start waiting again
        }
        // Timeout occurred, check if paused before performing auto-swap
        if (app_lcd_is_animation_paused()) {
            ESP_LOGD(TAG, "Auto-swap skipped: animation is paused");
            continue;  // Skip auto-swap while paused
        }
        // Perform auto-swap
        ESP_LOGD(TAG, "Auto-swap: cycling forward");
        app_lcd_cycle_animation();
    }
}

void auto_swap_reset_timer(void)
{
    if (s_auto_swap_task_handle != NULL) {
        xTaskNotifyGive(s_auto_swap_task_handle);
    }
}

static void register_rest_action_handlers(void)
{
    // Register action handlers for HTTP API swap commands
    http_api_set_action_handlers(
        app_lcd_cycle_animation,           // swap_next callback
        app_lcd_cycle_animation_backward   // swap_back callback
    );
    ESP_LOGI(TAG, "REST action handlers registered");
}

#if !DEBUG_PROVISIONING_ENABLED
static void makapix_state_monitor_task(void *arg)
{
    (void)arg;
    makapix_state_t last_state = MAKAPIX_STATE_IDLE;

    esp_err_t err = ugfx_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize µGFX UI: %s", esp_err_to_name(err));
        return;
    }

    while (true) {
        makapix_state_t current_state = makapix_get_state();

        if (current_state != last_state) {
            ESP_LOGI(TAG, "Makapix state changed: %d -> %d", last_state, current_state);

            // Handle state transitions
            if (current_state == MAKAPIX_STATE_SHOW_CODE) {
                // Enter UI mode and show registration code
                app_lcd_enter_ui_mode();

                char code[8];
                char expires[64];
                if (makapix_get_registration_code(code, sizeof(code)) == ESP_OK &&
                    makapix_get_registration_expires(expires, sizeof(expires)) == ESP_OK) {
                    // Show µGFX registration UI
                    esp_err_t err = ugfx_ui_show_registration(code, expires);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to show registration UI: %s", esp_err_to_name(err));
                    }
                    ESP_LOGI(TAG, "============================================");
                    ESP_LOGI(TAG, "   REGISTRATION CODE: %s", code);
                    ESP_LOGI(TAG, "   Expires: %s", expires);
                    ESP_LOGI(TAG, "   Enter at makapix.club");
                    ESP_LOGI(TAG, "============================================");
                }
            } else if (last_state == MAKAPIX_STATE_SHOW_CODE && current_state != MAKAPIX_STATE_SHOW_CODE) {
                // Exit UI mode FIRST, then hide registration
                // This ensures animation takes over immediately without an intermediate black frame
                app_lcd_exit_ui_mode();
                ugfx_ui_hide_registration();
                ESP_LOGI(TAG, "Registration mode exited");
            }

            last_state = current_state;
        }

        vTaskDelay(pdMS_TO_TICKS(500)); // Check every 500ms
    }
}
#endif

#if DEBUG_PROVISIONING_ENABLED
/**
 * @brief Debug task that toggles in/out of debug provisioning mode every 5 seconds
 * Does not make any API calls - displays mock registration code using µGFX
 */
static void debug_provisioning_task(void *arg)
{
    (void)arg;
    bool in_debug_mode = false;
    static const char *mock_code = "DBG123";
    static const char *mock_expires = "2099-12-31T23:59:59Z";

    ESP_LOGI(TAG, "Debug provisioning task started (toggle every %d ms)", DEBUG_PROVISIONING_TOGGLE_MS);

    // Wait for LCD to be initialized
    while (!app_lcd_get_panel_handle()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Initialize µGFX once (framebuffer will be set when entering UI mode)
    esp_err_t err = ugfx_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize µGFX UI: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "µGFX initialized, debug task ready");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DEBUG_PROVISIONING_TOGGLE_MS));

        in_debug_mode = !in_debug_mode;

        if (in_debug_mode) {
            ESP_LOGI(TAG, ">>> ENTERING DEBUG PROVISIONING MODE <<<");
            
            // Enter UI mode - this gets the framebuffer and sets it for µGFX
            err = app_lcd_enter_ui_mode();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enter UI mode: %s", esp_err_to_name(err));
                continue;
            }
            
            // Show µGFX registration screen
            err = ugfx_ui_show_registration(mock_code, mock_expires);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to show registration UI: %s", esp_err_to_name(err));
            }
            
            // Log mock registration info
            ESP_LOGI(TAG, "============================================");
            ESP_LOGI(TAG, "   [DEBUG] REGISTRATION CODE: %s", mock_code);
            ESP_LOGI(TAG, "   [DEBUG] Expires: %s", mock_expires);
            ESP_LOGI(TAG, "   Enter at makapix.club");
            ESP_LOGI(TAG, "============================================");
        } else {
            ESP_LOGI(TAG, ">>> EXITING DEBUG PROVISIONING MODE <<<");
            // Exit UI mode FIRST, then hide registration
            // This ensures animation takes over immediately without an intermediate black frame
            app_lcd_exit_ui_mode();
            ugfx_ui_hide_registration();
        }
    }
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "Starting p3a");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interface and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize SPIFFS filesystem
    esp_err_t fs_ret = fs_init();
    if (fs_ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS initialization failed: %s (continuing anyway)", esp_err_to_name(fs_ret));
    }

    // Initialize LCD and touch
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());

    ESP_ERROR_CHECK(app_usb_init());

    // Initialize Makapix module
    ESP_ERROR_CHECK(makapix_init());

    // Create auto-swap task
    const BaseType_t created = xTaskCreate(auto_swap_task, "auto_swap", 2048, NULL, 
                                           tskIDLE_PRIORITY + 1, &s_auto_swap_task_handle);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-swap task");
    }

    // Initialize Wi-Fi (will start captive portal if needed, or connect to saved network)
    ESP_ERROR_CHECK(app_wifi_init(register_rest_action_handlers));

#if DEBUG_PROVISIONING_ENABLED
    // Debug mode: toggle provisioning every 5 seconds without API calls
    ESP_LOGW(TAG, "DEBUG PROVISIONING MODE ENABLED - toggling every %d ms", DEBUG_PROVISIONING_TOGGLE_MS);
    xTaskCreate(debug_provisioning_task, "debug_prov", 4096, NULL, 5, NULL);
#else
    // Production: monitor real Makapix state and handle UI transitions
    xTaskCreate(makapix_state_monitor_task, "makapix_mon", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "p3a ready: tap the display to cycle animations (auto-swap forward every %d seconds)", AUTO_SWAP_INTERVAL_SECONDS);
}
