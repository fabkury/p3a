/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
#include "esp_heap_caps.h"
#endif

#include "app_lcd.h"
#include "app_touch.h"
#include "app_usb.h"
#include "app_wifi.h"
#include "http_api.h"
#include "p3a_board.h"  // For p3a_board_spiffs_mount()
#include "makapix.h"
#include "sntp_sync.h"
#include "ugfx_ui.h"
#include "animation_player.h"  // For animation_player_is_ui_mode()
#include "ota_manager.h"       // For OTA boot validation
#include "slave_ota.h"         // For ESP32-C6 co-processor OTA
#include "sdio_bus.h"          // SDIO bus coordination
#include "p3a_state.h"         // Unified p3a state machine
#include "p3a_render.h"        // State-aware rendering
#include "freertos/task.h"

// NVS namespace and keys for tracking firmware versions across boots
#define NVS_BOOT_NAMESPACE "p3a_boot"
#define NVS_LAST_P4_VERSION_KEY "last_p4_ver"
#define NVS_LAST_C6_VERSION_KEY "last_c6_ver"

static const char *TAG = "p3a";

// Debug provisioning mode - toggle every 5 seconds
#define DEBUG_PROVISIONING_ENABLED 0
#define DEBUG_PROVISIONING_TOGGLE_MS 5000

/**
 * @brief Check if this is the first boot after a firmware update and schedule reboot if needed
 * 
 * After flashing new firmware (especially with new co-processor firmware), the system
 * may need a "stabilization reboot" to ensure all hardware (particularly the ESP32-C6
 * co-processor connected via SDIO) is in a clean state.
 * 
 * This function checks BOTH:
 * - ESP32-P4 (host) firmware version
 * - ESP32-C6 (co-processor) embedded firmware version
 * 
 * If either version changes, a stabilization reboot is needed.
 * 
 * @return true if this is first boot after update (caller should reboot)
 * @return false if this is a normal boot (same versions)
 */
static bool check_first_boot_after_update(void)
{
    // Get current P4 (host) firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_p4_version = app_desc->version;
    
    // Get embedded C6 (co-processor) firmware version
    uint32_t c6_major = 0, c6_minor = 0, c6_patch = 0;
    slave_ota_get_embedded_version(&c6_major, &c6_minor, &c6_patch);
    char current_c6_version[32];
    snprintf(current_c6_version, sizeof(current_c6_version), "%lu.%lu.%lu", c6_major, c6_minor, c6_patch);
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_BOOT_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS for boot tracking: %s", esp_err_to_name(err));
        return false;
    }
    
    bool p4_changed = false;
    bool c6_changed = false;
    
    // Check P4 version
    char last_p4_version[32] = {0};
    size_t len = sizeof(last_p4_version);
    err = nvs_get_str(nvs, NVS_LAST_P4_VERSION_KEY, last_p4_version, &len);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "First P4 boot detected (no previous P4 version in NVS)");
        p4_changed = true;
    } else if (err == ESP_OK && strcmp(last_p4_version, current_p4_version) != 0) {
        ESP_LOGI(TAG, "P4 firmware changed: %s -> %s", last_p4_version, current_p4_version);
        p4_changed = true;
    }
    
    // Check C6 version
    char last_c6_version[32] = {0};
    len = sizeof(last_c6_version);
    err = nvs_get_str(nvs, NVS_LAST_C6_VERSION_KEY, last_c6_version, &len);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "First C6 boot detected (no previous C6 version in NVS)");
        c6_changed = true;
    } else if (err == ESP_OK && strcmp(last_c6_version, current_c6_version) != 0) {
        ESP_LOGI(TAG, "C6 firmware changed: %s -> %s", last_c6_version, current_c6_version);
        c6_changed = true;
    }
    
    bool needs_reboot = p4_changed || c6_changed;
    
    if (needs_reboot) {
        // Store current versions for next boot
        nvs_set_str(nvs, NVS_LAST_P4_VERSION_KEY, current_p4_version);
        nvs_set_str(nvs, NVS_LAST_C6_VERSION_KEY, current_c6_version);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Stored versions - P4: %s, C6: %s", current_p4_version, current_c6_version);
    }
    
    nvs_close(nvs);
    return needs_reboot;
}

#define AUTO_SWAP_INTERVAL_SECONDS CONFIG_P3A_AUTO_SWAP_INTERVAL_SECONDS
#define DEFAULT_DWELL_TIME_SECONDS 30
#define MIN_DWELL_TIME_SECONDS 1
#define MAX_DWELL_TIME_SECONDS 100000
#define DWELL_TIME_NVS_KEY "dwell_time"

#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
#define MEMORY_REPORT_INTERVAL_SECONDS 15
#endif

static TaskHandle_t s_auto_swap_task_handle = NULL;
static uint32_t s_dwell_time_seconds = DEFAULT_DWELL_TIME_SECONDS;
static SemaphoreHandle_t s_dwell_time_mutex = NULL;

static uint32_t load_dwell_time_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("p3a", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for dwell_time read: %s", esp_err_to_name(err));
        return DEFAULT_DWELL_TIME_SECONDS;
    }
    
    uint32_t dwell_time = DEFAULT_DWELL_TIME_SECONDS;
    err = nvs_get_u32(nvs, DWELL_TIME_NVS_KEY, &dwell_time);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No dwell_time in NVS, using default %u seconds", DEFAULT_DWELL_TIME_SECONDS);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read dwell_time from NVS: %s", esp_err_to_name(err));
    } else {
        // Validate range
        if (dwell_time < MIN_DWELL_TIME_SECONDS || dwell_time > MAX_DWELL_TIME_SECONDS) {
            ESP_LOGW(TAG, "Invalid dwell_time %u, using default %u", dwell_time, DEFAULT_DWELL_TIME_SECONDS);
            dwell_time = DEFAULT_DWELL_TIME_SECONDS;
        } else {
            ESP_LOGI(TAG, "Loaded dwell_time from NVS: %u seconds", dwell_time);
        }
    }
    
    nvs_close(nvs);
    return dwell_time;
}

static esp_err_t save_dwell_time_to_nvs(uint32_t dwell_time)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("p3a", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for dwell_time write: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_u32(nvs, DWELL_TIME_NVS_KEY, dwell_time);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write dwell_time to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }
    
    err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved dwell_time to NVS: %u seconds", dwell_time);
    } else {
        ESP_LOGE(TAG, "Failed to commit dwell_time to NVS: %s", esp_err_to_name(err));
    }
    
    return err;
}

uint32_t animation_player_get_dwell_time(void)
{
    if (s_dwell_time_mutex && xSemaphoreTake(s_dwell_time_mutex, portMAX_DELAY) == pdTRUE) {
        uint32_t dwell = s_dwell_time_seconds;
        xSemaphoreGive(s_dwell_time_mutex);
        return dwell;
    }
    return s_dwell_time_seconds;
}

esp_err_t animation_player_set_dwell_time(uint32_t dwell_time)
{
    if (dwell_time < MIN_DWELL_TIME_SECONDS || dwell_time > MAX_DWELL_TIME_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_dwell_time_mutex && xSemaphoreTake(s_dwell_time_mutex, portMAX_DELAY) == pdTRUE) {
        s_dwell_time_seconds = dwell_time;
        xSemaphoreGive(s_dwell_time_mutex);
    } else {
        s_dwell_time_seconds = dwell_time;
    }
    
    esp_err_t err = save_dwell_time_to_nvs(dwell_time);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save dwell_time to NVS, but using in-memory value");
    }
    
    // Notify auto-swap task to use new interval
    if (s_auto_swap_task_handle) {
        xTaskNotifyGive(s_auto_swap_task_handle);
    }
    
    ESP_LOGI(TAG, "Dwell time set to %u seconds", dwell_time);
    return ESP_OK;
}

static void auto_swap_task(void *arg)
{
    (void)arg;
    
    // Load dwell time from NVS
    s_dwell_time_seconds = load_dwell_time_from_nvs();
    
    ESP_LOGI(TAG, "Auto-swap task started: will cycle forward every %u seconds", s_dwell_time_seconds);
    
    // Wait a bit for system to initialize before first swap
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while (true) {
        // Get current dwell time (may have changed)
        uint32_t current_dwell = animation_player_get_dwell_time();
        const TickType_t delay_ticks = pdMS_TO_TICKS(current_dwell * 1000);
        
        // Wait for interval or notification (which resets the timer)
        uint32_t notified = ulTaskNotifyTake(pdTRUE, delay_ticks);
        if (notified > 0) {
            ESP_LOGD(TAG, "Auto-swap timer reset by user interaction or dwell_time change");
            continue;  // Timer was reset, start waiting again
        }
        // Timeout occurred, check if paused before performing auto-swap
        if (app_lcd_is_animation_paused()) {
            ESP_LOGD(TAG, "Auto-swap skipped: animation is paused");
            continue;  // Skip auto-swap while paused
        }
        // Check if in UI mode (e.g., during provisioning)
        if (animation_player_is_ui_mode()) {
            ESP_LOGD(TAG, "Auto-swap skipped: UI mode active");
            continue;  // Skip auto-swap during UI mode to avoid memory pressure
        }
        // Perform auto-swap
        ESP_LOGD(TAG, "Auto-swap: cycling forward");
        app_lcd_cycle_animation();
    }
}

#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
/**
 * @brief Memory reporting task that logs memory statistics every 15 seconds
 * 
 * Reports:
 * - Free heap memory (current and minimum since boot)
 * - Memory breakdown by capability (internal RAM, SPIRAM if available, etc.)
 * - Largest free block
 * - Number of FreeRTOS tasks
 */
static void memory_report_task(void *arg)
{
    (void)arg;
    const TickType_t delay_ticks = pdMS_TO_TICKS(MEMORY_REPORT_INTERVAL_SECONDS * 1000);
    
    ESP_LOGI(TAG, "Memory reporting task started: will report every %d seconds", MEMORY_REPORT_INTERVAL_SECONDS);
    
    // Wait a bit for system to initialize before first report
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (true) {
        // Get overall heap statistics
        size_t free_heap = esp_get_free_heap_size();
        size_t min_free_heap = esp_get_minimum_free_heap_size();
        size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        
        // Get memory breakdown by capability
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t used_internal = total_internal - free_internal;
        
        size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_spiram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t used_spiram = total_spiram - free_spiram;
        
        size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t total_dma = heap_caps_get_total_size(MALLOC_CAP_DMA);
        size_t used_dma = total_dma - free_dma;
        
        size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t total_8bit = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t used_8bit = total_8bit - free_8bit;
        
        // Get task count
        UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
        
        // Log memory report
        ESP_LOGI(TAG, "=== Memory Status Report ===");
        ESP_LOGI(TAG, "Overall Heap:");
        ESP_LOGI(TAG, "  Free: %zu bytes (%.2f KB)", free_heap, free_heap / 1024.0f);
        ESP_LOGI(TAG, "  Min Free (since boot): %zu bytes (%.2f KB)", min_free_heap, min_free_heap / 1024.0f);
        ESP_LOGI(TAG, "  Largest Free Block: %zu bytes (%.2f KB)", largest_free_block, largest_free_block / 1024.0f);
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Memory by Type:");
        ESP_LOGI(TAG, "  Internal RAM:");
        ESP_LOGI(TAG, "    Total: %zu bytes (%.2f KB)", total_internal, total_internal / 1024.0f);
        ESP_LOGI(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                 used_internal, used_internal / 1024.0f, 
                 total_internal > 0 ? (100.0f * used_internal / total_internal) : 0.0f);
        ESP_LOGI(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                 free_internal, free_internal / 1024.0f,
                 total_internal > 0 ? (100.0f * free_internal / total_internal) : 0.0f);
        
        if (total_spiram > 0) {
            ESP_LOGI(TAG, "  SPIRAM:");
            ESP_LOGI(TAG, "    Total: %zu bytes (%.2f KB)", total_spiram, total_spiram / 1024.0f);
            ESP_LOGI(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                     used_spiram, used_spiram / 1024.0f,
                     total_spiram > 0 ? (100.0f * used_spiram / total_spiram) : 0.0f);
            ESP_LOGI(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                     free_spiram, free_spiram / 1024.0f,
                     total_spiram > 0 ? (100.0f * free_spiram / total_spiram) : 0.0f);
        }
        
        if (total_dma > 0) {
            ESP_LOGI(TAG, "  DMA-Capable:");
            ESP_LOGI(TAG, "    Total: %zu bytes (%.2f KB)", total_dma, total_dma / 1024.0f);
            ESP_LOGI(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                     used_dma, used_dma / 1024.0f,
                     total_dma > 0 ? (100.0f * used_dma / total_dma) : 0.0f);
            ESP_LOGI(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                     free_dma, free_dma / 1024.0f,
                     total_dma > 0 ? (100.0f * free_dma / total_dma) : 0.0f);
        }
        
        ESP_LOGI(TAG, "  8-bit Accessible:");
        ESP_LOGI(TAG, "    Total: %zu bytes (%.2f KB)", total_8bit, total_8bit / 1024.0f);
        ESP_LOGI(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                 used_8bit, used_8bit / 1024.0f,
                 total_8bit > 0 ? (100.0f * used_8bit / total_8bit) : 0.0f);
        ESP_LOGI(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                 free_8bit, free_8bit / 1024.0f,
                 total_8bit > 0 ? (100.0f * free_8bit / total_8bit) : 0.0f);
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "System:");
        ESP_LOGI(TAG, "  FreeRTOS Tasks: %u", num_tasks);
        ESP_LOGI(TAG, "============================");
        
        vTaskDelay(delay_ticks);
    }
}
#endif // CONFIG_P3A_MEMORY_REPORTING_ENABLE

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
/**
 * @brief Monitor task that bridges makapix module states to unified p3a state machine
 * 
 * This task watches the makapix module's internal state and updates the unified
 * p3a state machine accordingly. It also handles the UI transitions for provisioning.
 */
static void makapix_state_monitor_task(void *arg)
{
    (void)arg;
    makapix_state_t last_makapix_state = MAKAPIX_STATE_IDLE;

    esp_err_t err = ugfx_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize µGFX UI: %s", esp_err_to_name(err));
        return;
    }

    while (true) {
        makapix_state_t current_makapix_state = makapix_get_state();

        if (current_makapix_state != last_makapix_state) {
            ESP_LOGI(TAG, "Makapix state changed: %d -> %d", last_makapix_state, current_makapix_state);

            // Handle state transitions - sync with unified p3a state machine
            if (current_makapix_state == MAKAPIX_STATE_PROVISIONING) {
                // Transition p3a to provisioning state if allowed
                if (p3a_state_get() == P3A_STATE_ANIMATION_PLAYBACK) {
                    p3a_state_enter_provisioning();
                }
                p3a_state_set_provisioning_substate(P3A_PROV_STATUS);
                
                // Enter UI mode immediately and show status message
                app_lcd_enter_ui_mode();

                char status[128];
                if (makapix_get_provisioning_status(status, sizeof(status)) == ESP_OK) {
                    p3a_render_set_provisioning_status(status);
                    esp_err_t show_err = ugfx_ui_show_provisioning_status(status);
                    if (show_err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to show provisioning status UI: %s", esp_err_to_name(show_err));
                    }
                } else {
                    // Default status message
                    p3a_render_set_provisioning_status("Starting...");
                    esp_err_t show_err = ugfx_ui_show_provisioning_status("Starting...");
                    if (show_err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to show provisioning status UI: %s", esp_err_to_name(show_err));
                    }
                }
                ESP_LOGI(TAG, "Provisioning UI displayed");
                
            } else if (current_makapix_state == MAKAPIX_STATE_SHOW_CODE) {
                // Update p3a provisioning sub-state
                p3a_state_set_provisioning_substate(P3A_PROV_SHOW_CODE);
                
                // Already in UI mode from PROVISIONING, just update to show code
                char code[8];
                char expires[64];
                if (makapix_get_registration_code(code, sizeof(code)) == ESP_OK &&
                    makapix_get_registration_expires(expires, sizeof(expires)) == ESP_OK) {
                    // Update render state
                    p3a_render_set_provisioning_code(code, expires);
                    
                    // Show µGFX registration UI
                    esp_err_t show_err = ugfx_ui_show_registration(code, expires);
                    if (show_err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to show registration UI: %s", esp_err_to_name(show_err));
                    }
                    ESP_LOGI(TAG, "============================================");
                    ESP_LOGI(TAG, "   REGISTRATION CODE: %s", code);
                    ESP_LOGI(TAG, "   Expires: %s", expires);
                    ESP_LOGI(TAG, "   Enter at makapix.club");
                    ESP_LOGI(TAG, "============================================");
                }
                
            } else if ((last_makapix_state == MAKAPIX_STATE_PROVISIONING || last_makapix_state == MAKAPIX_STATE_SHOW_CODE) && 
                       current_makapix_state != MAKAPIX_STATE_PROVISIONING && current_makapix_state != MAKAPIX_STATE_SHOW_CODE) {
                // Transition back to animation playback
                p3a_state_exit_to_playback();
                
                // Exit UI mode FIRST, then hide registration
                // This ensures animation takes over immediately without an intermediate black frame
                app_lcd_exit_ui_mode();
                ugfx_ui_hide_registration();
                ESP_LOGI(TAG, "Registration mode exited");
            }

            last_makapix_state = current_makapix_state;
        } else if (current_makapix_state == MAKAPIX_STATE_PROVISIONING) {
            // Update status message during provisioning
            char status[128];
            if (makapix_get_provisioning_status(status, sizeof(status)) == ESP_OK) {
                p3a_render_set_provisioning_status(status);
                ugfx_ui_show_provisioning_status(status);
            }
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

    // Initialize SDIO bus coordinator early
    // This provides mutual exclusion for SDIO operations (WiFi and SD card)
    esp_err_t sdio_err = sdio_bus_init();
    if (sdio_err != ESP_OK) {
        ESP_LOGW(TAG, "SDIO bus coordinator init failed: %s (continuing anyway)", esp_err_to_name(sdio_err));
    }

    // Initialize unified p3a state machine (must be after NVS)
    // This loads the remembered channel and sets initial state
    esp_err_t state_err = p3a_state_init();
    if (state_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize p3a state machine: %s", esp_err_to_name(state_err));
        // Continue anyway - state machine will use defaults
    }

    // Validate OTA boot early - this must be done before any complex operations
    // If running a new OTA firmware, this marks it as valid to prevent rollback
    esp_err_t ota_err = ota_manager_validate_boot();
    if (ota_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA boot validation issue: %s", esp_err_to_name(ota_err));
    }

    // Initialize network interface and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize SPIFFS filesystem
    esp_err_t fs_ret = p3a_board_spiffs_mount();
    if (fs_ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS initialization failed: %s (continuing anyway)", esp_err_to_name(fs_ret));
    }

    // Initialize LCD and touch
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());

    // Initialize state-aware rendering (after display is ready)
    esp_err_t render_err = p3a_render_init();
    if (render_err != ESP_OK) {
        ESP_LOGW(TAG, "p3a_render_init failed: %s (continuing anyway)", esp_err_to_name(render_err));
    }

    ESP_ERROR_CHECK(app_usb_init());

    // Initialize Makapix module
    ESP_ERROR_CHECK(makapix_init());

    // Initialize dwell time mutex
    s_dwell_time_mutex = xSemaphoreCreateMutex();
    if (!s_dwell_time_mutex) {
        ESP_LOGE(TAG, "Failed to create dwell_time mutex");
    }

    // Create auto-swap task (4096 bytes needed for channel_player calls to Makapix channels)
    const BaseType_t created = xTaskCreate(auto_swap_task, "auto_swap", 4096, NULL, 
                                           tskIDLE_PRIORITY + 1, &s_auto_swap_task_handle);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-swap task");
    }

#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
    // Create memory reporting task
    const BaseType_t mem_task_created = xTaskCreate(memory_report_task, "mem_report", 3072, NULL,
                                                    tskIDLE_PRIORITY + 1, NULL);
    if (mem_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create memory reporting task");
    }
#endif // CONFIG_P3A_MEMORY_REPORTING_ENABLE

    // Check if this is first boot after firmware update
    bool needs_stabilization_reboot = check_first_boot_after_update();
    
    // Initialize Wi-Fi (will start captive portal if needed, or connect to saved network)
    ESP_ERROR_CHECK(app_wifi_init(register_rest_action_handlers));

    // Check and update ESP32-C6 co-processor firmware if needed
    // This uses the ESP-Hosted OTA feature to update the WiFi chip
    esp_err_t slave_ota_err = slave_ota_check_and_update();
    if (slave_ota_err != ESP_OK && slave_ota_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Slave OTA check failed: %s (continuing anyway)", esp_err_to_name(slave_ota_err));
    }
    
    // After WiFi init, if this was first boot after update, do a stabilization reboot
    // This ensures the ESP32-C6 co-processor and SDIO bus are in a clean state
    if (needs_stabilization_reboot) {
        ESP_LOGW(TAG, "First boot after firmware update - performing stabilization reboot...");
        ESP_LOGI(TAG, "This ensures co-processor and SDIO bus are properly initialized");
        vTaskDelay(pdMS_TO_TICKS(2000));  // Brief delay so user can see the message
        esp_restart();
        // Won't reach here
    }

    // Initialize OTA manager - starts periodic update checks
    // (checks are skipped if WiFi is not connected)
    esp_err_t ota_init_err = ota_manager_init();
    if (ota_init_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA manager init failed: %s (OTA updates disabled)", esp_err_to_name(ota_init_err));
    }

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
