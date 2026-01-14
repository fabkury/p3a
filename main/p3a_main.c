/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
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
#include "p3a_board.h"  // For p3a_board_littlefs_mount()
#include "makapix.h"
#include "sntp_sync.h"
#include "ugfx_ui.h"
#include "animation_player.h"  // For animation_player_is_ui_mode()
#include "play_scheduler.h"
#include "config_store.h"
#include "ota_manager.h"       // For OTA boot validation
#include "slave_ota.h"         // For ESP32-C6 co-processor OTA
#include "sdio_bus.h"          // SDIO bus coordination
#include "p3a_state.h"         // Unified p3a state machine
#include "p3a_render.h"        // State-aware rendering
#include "swap_future.h"       // Live Mode swap_future
#include "live_mode.h"         // Live Mode time helpers
#include "fresh_boot.h"        // Fresh boot debug helpers
#include "connectivity_state.h" // Hierarchical connectivity state machine
#include "channel_cache.h"       // LAi persistence for channel caches
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
        ESP_LOGD(TAG, "First P4 boot detected (no previous P4 version in NVS)");
        p4_changed = true;
    } else if (err == ESP_OK && strcmp(last_p4_version, current_p4_version) != 0) {
        ESP_LOGD(TAG, "P4 firmware changed: %s -> %s", last_p4_version, current_p4_version);
        p4_changed = true;
    }
    
    // Check C6 version
    char last_c6_version[32] = {0};
    len = sizeof(last_c6_version);
    err = nvs_get_str(nvs, NVS_LAST_C6_VERSION_KEY, last_c6_version, &len);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "First C6 boot detected (no previous C6 version in NVS)");
        c6_changed = true;
    } else if (err == ESP_OK && strcmp(last_c6_version, current_c6_version) != 0) {
        ESP_LOGD(TAG, "C6 firmware changed: %s -> %s", last_c6_version, current_c6_version);
        c6_changed = true;
    }
    
    bool needs_reboot = p4_changed || c6_changed;
    
    if (needs_reboot) {
        // Store current versions for next boot
        nvs_set_str(nvs, NVS_LAST_P4_VERSION_KEY, current_p4_version);
        nvs_set_str(nvs, NVS_LAST_C6_VERSION_KEY, current_c6_version);
        nvs_commit(nvs);
        ESP_LOGD(TAG, "Stored versions - P4: %s, C6: %s", current_p4_version, current_c6_version);
    }
    
    nvs_close(nvs);
    return needs_reboot;
}

// Dwell time management delegates to play_scheduler

#define MAX_DWELL_TIME_SECONDS 100000

#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
#define MEMORY_REPORT_INTERVAL_SECONDS 15
#endif

uint32_t animation_player_get_dwell_time(void)
{
    return play_scheduler_get_dwell_time();
}

esp_err_t animation_player_set_dwell_time(uint32_t dwell_time)
{
    if (dwell_time > MAX_DWELL_TIME_SECONDS) {
        return ESP_ERR_INVALID_ARG;
    }

    // Store in config
    esp_err_t err = config_store_set_dwell_time(dwell_time * 1000u);
    if (err != ESP_OK) return err;

    // Update play_scheduler
    play_scheduler_set_dwell_time(dwell_time);
    return ESP_OK;
}

// Phase 7: auto_swap_task removed - timer task now in play_scheduler

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
    
    ESP_LOGD(TAG, "Memory reporting task started: will report every %d seconds", MEMORY_REPORT_INTERVAL_SECONDS);
    
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
        
        // Log memory report (debug level - periodic monitoring)
        ESP_LOGD(TAG, "=== Memory Status Report ===");
        ESP_LOGD(TAG, "Overall Heap:");
        ESP_LOGD(TAG, "  Free: %zu bytes (%.2f KB)", free_heap, free_heap / 1024.0f);
        ESP_LOGD(TAG, "  Min Free (since boot): %zu bytes (%.2f KB)", min_free_heap, min_free_heap / 1024.0f);
        ESP_LOGD(TAG, "  Largest Free Block: %zu bytes (%.2f KB)", largest_free_block, largest_free_block / 1024.0f);
        ESP_LOGD(TAG, "");
        ESP_LOGD(TAG, "Memory by Type:");
        ESP_LOGD(TAG, "  Internal RAM:");
        ESP_LOGD(TAG, "    Total: %zu bytes (%.2f KB)", total_internal, total_internal / 1024.0f);
        ESP_LOGD(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                 used_internal, used_internal / 1024.0f, 
                 total_internal > 0 ? (100.0f * used_internal / total_internal) : 0.0f);
        ESP_LOGD(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                 free_internal, free_internal / 1024.0f,
                 total_internal > 0 ? (100.0f * free_internal / total_internal) : 0.0f);
        
        if (total_spiram > 0) {
            ESP_LOGD(TAG, "  SPIRAM:");
            ESP_LOGD(TAG, "    Total: %zu bytes (%.2f KB)", total_spiram, total_spiram / 1024.0f);
            ESP_LOGD(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                     used_spiram, used_spiram / 1024.0f,
                     total_spiram > 0 ? (100.0f * used_spiram / total_spiram) : 0.0f);
            ESP_LOGD(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                     free_spiram, free_spiram / 1024.0f,
                     total_spiram > 0 ? (100.0f * free_spiram / total_spiram) : 0.0f);
        }
        
        if (total_dma > 0) {
            ESP_LOGD(TAG, "  DMA-Capable:");
            ESP_LOGD(TAG, "    Total: %zu bytes (%.2f KB)", total_dma, total_dma / 1024.0f);
            ESP_LOGD(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                     used_dma, used_dma / 1024.0f,
                     total_dma > 0 ? (100.0f * used_dma / total_dma) : 0.0f);
            ESP_LOGD(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                     free_dma, free_dma / 1024.0f,
                     total_dma > 0 ? (100.0f * free_dma / total_dma) : 0.0f);
        }
        
        ESP_LOGD(TAG, "  8-bit Accessible:");
        ESP_LOGD(TAG, "    Total: %zu bytes (%.2f KB)", total_8bit, total_8bit / 1024.0f);
        ESP_LOGD(TAG, "    Used: %zu bytes (%.2f KB, %.1f%%)", 
                 used_8bit, used_8bit / 1024.0f,
                 total_8bit > 0 ? (100.0f * used_8bit / total_8bit) : 0.0f);
        ESP_LOGD(TAG, "    Free: %zu bytes (%.2f KB, %.1f%%)", 
                 free_8bit, free_8bit / 1024.0f,
                 total_8bit > 0 ? (100.0f * free_8bit / total_8bit) : 0.0f);
        
        ESP_LOGD(TAG, "");
        ESP_LOGD(TAG, "System:");
        ESP_LOGD(TAG, "  FreeRTOS Tasks: %u", num_tasks);
        ESP_LOGD(TAG, "============================");
        
        vTaskDelay(delay_ticks);
    }
}
#endif // CONFIG_P3A_MEMORY_REPORTING_ENABLE

static void register_rest_action_handlers(void)
{
    // Register action handlers for HTTP API swap commands
    http_api_set_action_handlers(
        app_lcd_cycle_animation,           // swap_next callback
        app_lcd_cycle_animation_backward   // swap_back callback
    );
    ESP_LOGD(TAG, "REST action handlers registered");
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
            ESP_LOGD(TAG, "Makapix state changed: %d -> %d", last_makapix_state, current_makapix_state);

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
                ESP_LOGD(TAG, "Provisioning UI displayed");
                
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
                // Check if cleanup was already done synchronously by touch router
                // (when user long-pressed to cancel provisioning)
                bool still_in_ui = app_lcd_is_ui_mode();

                if (still_in_ui) {
                    // Touch router didn't clean up (e.g., credentials received vs cancelled by user)
                    // Transition back to animation playback
                    p3a_state_exit_to_playback();

                    // Exit UI mode FIRST, then hide registration
                    // This ensures animation takes over immediately without an intermediate black frame
                    app_lcd_exit_ui_mode();
                    ugfx_ui_hide_registration();
                }
                ESP_LOGD(TAG, "Registration mode exited (cleanup was %s)", still_in_ui ? "needed" : "already done");
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

    ESP_LOGD(TAG, "Debug provisioning task started (toggle every %d ms)", DEBUG_PROVISIONING_TOGGLE_MS);

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
    ESP_LOGD(TAG, "µGFX initialized, debug task ready");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DEBUG_PROVISIONING_TOGGLE_MS));

        in_debug_mode = !in_debug_mode;

        if (in_debug_mode) {
            ESP_LOGD(TAG, ">>> ENTERING DEBUG PROVISIONING MODE <<<");
            
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
            ESP_LOGD(TAG, "[DEBUG] Registration code: %s (expires: %s)", mock_code, mock_expires);
        } else {
            ESP_LOGD(TAG, ">>> EXITING DEBUG PROVISIONING MODE <<<");
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

#if CONFIG_P3A_FORCE_FRESH_NVS
    // Debug: Erase p3a NVS namespaces to simulate fresh boot
    ESP_LOGW(TAG, "CONFIG_P3A_FORCE_FRESH_NVS enabled - erasing p3a NVS namespaces");
    fresh_boot_erase_nvs();
#endif

    // Set timezone to UTC for Live Mode synchronization
    setenv("TZ", "UTC", 1);
    tzset();
    ESP_LOGD(TAG, "Timezone set to UTC for Live Mode");

    // Initialize random seed from hardware RNG
    uint32_t random_seed = esp_random();
    config_store_set_effective_seed(random_seed);
    ESP_LOGD(TAG, "Random seed initialized: 0x%08x", random_seed);

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

    // Initialize connectivity state tracking (hierarchical WiFi→Internet→Registration→MQTT)
    esp_err_t conn_err = connectivity_state_init();
    if (conn_err != ESP_OK) {
        ESP_LOGW(TAG, "connectivity_state_init failed: %s", esp_err_to_name(conn_err));
    }

    // Initialize channel cache subsystem (LAi persistence, debounced saves)
    esp_err_t cache_err = channel_cache_init();
    if (cache_err != ESP_OK) {
        ESP_LOGW(TAG, "channel_cache_init failed: %s", esp_err_to_name(cache_err));
    }

    // Initialize play_scheduler (the deterministic playback engine)
    esp_err_t ps_err = play_scheduler_init();
    if (ps_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize play_scheduler: %s", esp_err_to_name(ps_err));
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

    // Initialize LittleFS filesystem
    esp_err_t fs_ret = p3a_board_littlefs_mount();
    if (fs_ret != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS initialization failed: %s (continuing anyway)", esp_err_to_name(fs_ret));
    }

    // Initialize Makapix module early (after LittleFS mount, before animation player/channel load).
    // This ensures Makapix API layer is ready before any Makapix channel refresh tasks may start.
    ESP_ERROR_CHECK(makapix_init());

    // Initialize LCD and touch
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_touch_init());

    // Initialize state-aware rendering (after display is ready)
    esp_err_t render_err = p3a_render_init();
    if (render_err != ESP_OK) {
        ESP_LOGW(TAG, "p3a_render_init failed: %s (continuing anyway)", esp_err_to_name(render_err));
    }

    ESP_ERROR_CHECK(app_usb_init());

    // Phase 7: auto_swap_task removed - timer task now in play_scheduler

#if CONFIG_P3A_MEMORY_REPORTING_ENABLE
    // Create memory reporting task
    const BaseType_t mem_task_created = xTaskCreate(memory_report_task, "mem_report", 3072, NULL,
                                                    tskIDLE_PRIORITY + 1, NULL);
    if (mem_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create memory reporting task");
    }
#endif // CONFIG_P3A_MEMORY_REPORTING_ENABLE

    // Check if this is first boot after firmware update
    // Skip this check when FORCE_FRESH_NVS is enabled, as we intentionally erased
    // the p3a_boot namespace and don't want an infinite reboot loop
#if CONFIG_P3A_FORCE_FRESH_NVS
    bool needs_stabilization_reboot = false;
    ESP_LOGW(TAG, "Skipping stabilization reboot check (CONFIG_P3A_FORCE_FRESH_NVS enabled)");
#else
    bool needs_stabilization_reboot = check_first_boot_after_update();
#endif
    
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
        ESP_LOGD(TAG, "This ensures co-processor and SDIO bus are properly initialized");
        vTaskDelay(pdMS_TO_TICKS(2000));  // Brief delay so user can see the message
        esp_restart();
        // Won't reach here
    }

    // NOTE: boot-time channel restore happens earlier during animation player initialization,
    // so the first animation shown is already from the last remembered channel.

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

    ESP_LOGI(TAG, "p3a ready: tap the display to cycle animations (auto-swap enabled)");
}
