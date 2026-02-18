// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file ota_manager.c
 * @brief OTA Manager - Core lifecycle, state management, and update checks
 */

#include "ota_manager_internal.h"
#include "sdio_bus.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include <string.h>

static const char *TAG = "ota_manager";

// PSRAM-backed stack for OTA check task
static StackType_t *s_ota_check_stack = NULL;
static StaticTask_t s_ota_check_task_buffer;

// Check interval in microseconds (hours to us)
#define CHECK_INTERVAL_US (CONFIG_OTA_CHECK_INTERVAL_HOURS * 60ULL * 60ULL * 1000000ULL)

// Web UI OTA max consecutive failures before disabling auto-update
#define WEBUI_OTA_MAX_FAILURES 4

// OTA state
ota_internal_state_t s_ota = {
    .state = OTA_STATE_IDLE,
    .initialized = false,
    .ui_active = false
};

#if CONFIG_P3A_PICO8_ENABLE
extern bool pico8_stream_is_active(void);
#else
static inline bool pico8_stream_is_active(void) { return false; }
#endif

// Retry parameters for OTA check when animation loader is busy
#define OTA_CHECK_RETRY_DELAY_MS    5000
#define OTA_CHECK_MAX_RETRIES       6

// Forward declarations
static void ota_check_task(void *arg);
static void ota_timer_callback(void *arg);

const char *ota_state_to_string(ota_state_t state)
{
    switch (state) {
        case OTA_STATE_IDLE:              return "idle";
        case OTA_STATE_CHECKING:          return "checking";
        case OTA_STATE_UPDATE_AVAILABLE:  return "update_available";
        case OTA_STATE_DOWNLOADING:       return "downloading";
        case OTA_STATE_VERIFYING:         return "verifying";
        case OTA_STATE_FLASHING:          return "flashing";
        case OTA_STATE_PENDING_REBOOT:    return "pending_reboot";
        case OTA_STATE_ERROR:             return "error";
        default:                          return "unknown";
    }
}

void set_state(ota_state_t new_state)
{
    if (s_ota.mutex) {
        xSemaphoreTake(s_ota.mutex, portMAX_DELAY);
    }
    s_ota.state = new_state;
    ESP_LOGI(TAG, "OTA state: %s", ota_state_to_string(new_state));
    if (s_ota.mutex) {
        xSemaphoreGive(s_ota.mutex);
    }

    // Sync with unified p3a state machine
    p3a_ota_substate_t p3a_substate;
    switch (new_state) {
        case OTA_STATE_CHECKING:
            p3a_substate = P3A_OTA_CHECKING;
            break;
        case OTA_STATE_DOWNLOADING:
            p3a_substate = P3A_OTA_DOWNLOADING;
            break;
        case OTA_STATE_VERIFYING:
            p3a_substate = P3A_OTA_VERIFYING;
            break;
        case OTA_STATE_FLASHING:
            p3a_substate = P3A_OTA_FLASHING;
            break;
        case OTA_STATE_PENDING_REBOOT:
            p3a_substate = P3A_OTA_PENDING_REBOOT;
            break;
        default:
            // Don't update substate for IDLE, UPDATE_AVAILABLE, ERROR
            return;
    }
    p3a_state_set_ota_substate(p3a_substate);
}

void set_error(const char *message)
{
    if (s_ota.mutex) {
        xSemaphoreTake(s_ota.mutex, portMAX_DELAY);
    }
    s_ota.state = OTA_STATE_ERROR;
    strncpy(s_ota.error_message, message, sizeof(s_ota.error_message) - 1);
    s_ota.error_message[sizeof(s_ota.error_message) - 1] = '\0';
    ESP_LOGE(TAG, "OTA error: %s", message);
    if (s_ota.mutex) {
        xSemaphoreGive(s_ota.mutex);
    }
}

void set_progress(int percent, const char *status)
{
    if (s_ota.mutex) {
        xSemaphoreTake(s_ota.mutex, portMAX_DELAY);
    }
    s_ota.download_progress = percent;
    if (s_ota.mutex) {
        xSemaphoreGive(s_ota.mutex);
    }

    // Update unified p3a state machine
    p3a_state_set_ota_progress(percent, status);
    p3a_render_set_ota_progress(percent, status, NULL, NULL);

    if (s_ota.progress_callback) {
        s_ota.progress_callback(percent, status);
    }
}

void ota_exit_ui_mode(void)
{
    if (s_ota.ui_active && s_ota.ui_callback) {
        s_ota.ui_callback(false, NULL, NULL);
        s_ota.ui_active = false;
    }
}

bool ota_manager_is_blocked(const char **reason)
{
    #if CONFIG_P3A_PICO8_ENABLE
    if (pico8_stream_is_active()) {
        if (reason) *reason = "PICO-8 streaming active";
        return true;
    }
    #endif

    if (animation_player_is_sd_export_locked()) {
        if (reason) *reason = "USB mass storage active";
        return true;
    }

    if (s_ota.state == OTA_STATE_DOWNLOADING ||
        s_ota.state == OTA_STATE_VERIFYING ||
        s_ota.state == OTA_STATE_FLASHING) {
        if (reason) *reason = "OTA already in progress";
        return true;
    }

    return false;
}

bool ota_manager_is_checking(void)
{
    return s_ota.state == OTA_STATE_CHECKING;
}

esp_err_t ota_check_wifi_connected(void)
{
    // Try both interface keys (local WiFi and remote via ESP32-C6)
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_RMT");
    }
    if (!netif) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

esp_err_t ota_manager_init(void)
{
    if (s_ota.initialized) {
        return ESP_OK;
    }

    s_ota.mutex = xSemaphoreCreateMutex();
    if (!s_ota.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Initialize web UI OTA subsystem
    esp_err_t webui_err = webui_ota_init();
    if (webui_err != ESP_OK) {
        vSemaphoreDelete(s_ota.mutex);
        s_ota.mutex = NULL;
        return webui_err;
    }

    // Create periodic check timer
    esp_timer_create_args_t timer_args = {
        .callback = ota_timer_callback,
        .name = "ota_check",
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_ota.check_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(err));
        webui_ota_deinit();
        vSemaphoreDelete(s_ota.mutex);
        s_ota.mutex = NULL;
        return err;
    }

    // Start periodic timer
    err = esp_timer_start_periodic(s_ota.check_timer, CHECK_INTERVAL_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_ota.check_timer);
        webui_ota_deinit();
        vSemaphoreDelete(s_ota.mutex);
        s_ota.mutex = NULL;
        return err;
    }

    s_ota.initialized = true;
#if CONFIG_OTA_DEV_MODE
    ESP_LOGW(TAG, "OTA manager initialized in DEVELOPMENT MODE (pre-releases enabled, check interval: %d hours)", CONFIG_OTA_CHECK_INTERVAL_HOURS);
#else
    ESP_LOGI(TAG, "OTA manager initialized (check interval: %d hours)", CONFIG_OTA_CHECK_INTERVAL_HOURS);
#endif

    // Do initial check after a short delay
    esp_timer_handle_t initial_check_timer;
    esp_timer_create_args_t initial_args = {
        .callback = ota_timer_callback,
        .name = "ota_initial",
    };
    if (esp_timer_create(&initial_args, &initial_check_timer) == ESP_OK) {
        esp_timer_start_once(initial_check_timer, 300 * 1000000);  // 5 minutes after boot
    }

    return ESP_OK;
}

void ota_manager_deinit(void)
{
    if (!s_ota.initialized) {
        return;
    }

    if (s_ota.check_timer) {
        esp_timer_stop(s_ota.check_timer);
        esp_timer_delete(s_ota.check_timer);
        s_ota.check_timer = NULL;
    }

    webui_ota_deinit();

    if (s_ota.mutex) {
        vSemaphoreDelete(s_ota.mutex);
        s_ota.mutex = NULL;
    }

    s_ota.initialized = false;
    ESP_LOGI(TAG, "OTA manager deinitialized");
}

ota_state_t ota_manager_get_state(void)
{
    return s_ota.state;
}

esp_err_t ota_manager_get_status(ota_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota.mutex) {
        xSemaphoreTake(s_ota.mutex, portMAX_DELAY);
    }

    memset(status, 0, sizeof(ota_status_t));

    status->state = s_ota.state;
    status->last_check_time = s_ota.last_check_time;
    status->download_progress = s_ota.download_progress;
    snprintf(status->error_message, sizeof(status->error_message), "%s", s_ota.error_message);

    // Get current running version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        snprintf(status->current_version, sizeof(status->current_version), "%s", app_desc->version);
    }

    // Copy available release info if we have it
    if (s_ota.state == OTA_STATE_UPDATE_AVAILABLE ||
        s_ota.state == OTA_STATE_DOWNLOADING ||
        s_ota.state == OTA_STATE_VERIFYING ||
        s_ota.state == OTA_STATE_FLASHING) {
        snprintf(status->available_version, sizeof(status->available_version), "%s", s_ota.release_info.version);
        status->available_size = s_ota.release_info.firmware_size;
        snprintf(status->release_notes, sizeof(status->release_notes), "%s", s_ota.release_info.release_notes);
    }

    // Check if rollback is available
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);
    if (other) {
        esp_app_desc_t other_app_desc;
        if (esp_ota_get_partition_description(other, &other_app_desc) == ESP_OK) {
            status->can_rollback = true;
            snprintf(status->rollback_version, sizeof(status->rollback_version), "%s", other_app_desc.version);
        }
    }

    // Dev mode status
#if CONFIG_OTA_DEV_MODE
    status->dev_mode = true;
#else
    status->dev_mode = false;
#endif
    status->is_prerelease = s_ota.release_info.is_prerelease;

    if (s_ota.mutex) {
        xSemaphoreGive(s_ota.mutex);
    }

    return ESP_OK;
}

static void ota_timer_callback(void *arg)
{
    (void)arg;

    // Don't start check if one is already in progress
    if (s_ota.state == OTA_STATE_CHECKING) {
        return;
    }

    ota_manager_check_for_update();
}

static void ota_check_task(void *arg)
{
    (void)arg;

    // Skip OTA check if device is not in regular animation playback mode
    if (animation_player_is_ui_mode()) {
        ESP_LOGW(TAG, "Skipping OTA check: UI mode active");
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (animation_player_is_sd_export_locked()) {
        ESP_LOGW(TAG, "Skipping OTA check: SD card exported over USB");
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (pico8_stream_is_active()) {
        ESP_LOGW(TAG, "Skipping OTA check: PICO-8 streaming active");
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Wait for animation loader to be idle before acquiring SDIO bus
    int retry_count = 0;
    while (animation_player_is_loader_busy() && retry_count < OTA_CHECK_MAX_RETRIES) {
        retry_count++;
        ESP_LOGI(TAG, "Animation loader busy, waiting %d ms before OTA check (attempt %d/%d)",
                 OTA_CHECK_RETRY_DELAY_MS, retry_count, OTA_CHECK_MAX_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_RETRY_DELAY_MS));
    }

    if (retry_count >= OTA_CHECK_MAX_RETRIES && animation_player_is_loader_busy()) {
        ESP_LOGW(TAG, "Animation loader still busy after %d retries, skipping OTA check", OTA_CHECK_MAX_RETRIES);
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // CRITICAL: Acquire exclusive SDIO bus access to avoid bus contention
    // The ESP32-P4 shares SDMMC controller between WiFi (SDIO Slot 1) and SD card (Slot 0)
    // High-bandwidth WiFi operations can conflict with SD card access
    ESP_LOGI(TAG, "Acquiring SDIO bus for OTA check...");
    esp_err_t bus_err = sdio_bus_acquire(10000, "OTA_CHECK");  // 10s timeout
    if (bus_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to acquire SDIO bus, skipping OTA check");
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Also pause SD access to prevent new file operations
    animation_player_pause_sd_access();

    // Wait for SDIO bus to fully settle
    // The ESP Hosted driver needs time to flush any pending operations
    ESP_LOGI(TAG, "Waiting for SDIO bus to settle (0.5s)...");
    vTaskDelay(pdMS_TO_TICKS(500));

    set_state(OTA_STATE_CHECKING);

    // Check WiFi first
    if (ota_check_wifi_connected() != ESP_OK) {
        ESP_LOGW(TAG, "No WiFi connection, skipping update check");
        animation_player_resume_sd_access();
        sdio_bus_release();
        set_state(OTA_STATE_IDLE);
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Fetch latest release from GitHub with retry logic
    github_release_info_t release_info;
    esp_err_t err = ESP_FAIL;
    const int max_github_retries = 3;

    for (int github_attempt = 1; github_attempt <= max_github_retries; github_attempt++) {
        err = github_ota_get_latest_release(&release_info);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            // Success or no releases found - don't retry
            break;
        }

        if (github_attempt < max_github_retries) {
            ESP_LOGW(TAG, "GitHub API call failed (attempt %d/%d): %s. Retrying in 3s...",
                     github_attempt, max_github_retries, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            ESP_LOGE(TAG, "GitHub API call failed after %d attempts: %s",
                     max_github_retries, esp_err_to_name(err));
        }
    }

    // Release SDIO bus and resume SD access after GitHub API call completes
    animation_player_resume_sd_access();
    sdio_bus_release();

    if (s_ota.mutex) {
        xSemaphoreTake(s_ota.mutex, portMAX_DELAY);
    }
    s_ota.last_check_time = esp_timer_get_time() / 1000000;  // Convert to seconds
    if (s_ota.mutex) {
        xSemaphoreGive(s_ota.mutex);
    }

    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "No releases found on GitHub");
        } else {
            ESP_LOGW(TAG, "Failed to fetch release info: %s", esp_err_to_name(err));
        }
        set_state(OTA_STATE_IDLE);
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Note: The GitHub API client already filters releases based on dev mode
    // In dev mode, it returns the first prerelease (or falls back to regular)
    // In production mode, it only returns regular releases

    // Compare versions
    const esp_app_desc_t *current_app = esp_app_get_description();
    if (!current_app) {
        set_error("Failed to get current app info");
        s_ota.check_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int cmp = github_ota_compare_versions(release_info.version, current_app->version);

    if (cmp > 0) {
        ESP_LOGI(TAG, "Update available: %s -> %s", current_app->version, release_info.version);

        if (s_ota.mutex) {
            xSemaphoreTake(s_ota.mutex, portMAX_DELAY);
        }
        memcpy(&s_ota.release_info, &release_info, sizeof(release_info));
        if (s_ota.mutex) {
            xSemaphoreGive(s_ota.mutex);
        }

        set_state(OTA_STATE_UPDATE_AVAILABLE);
    } else {
        ESP_LOGI(TAG, "Firmware is up to date (current: %s, latest: %s)",
                 current_app->version, release_info.version);
        set_state(OTA_STATE_IDLE);
    }

#if CONFIG_OTA_WEBUI_ENABLE
    // Check for web UI updates (after firmware check)
    // Use manifest API to get web UI version info
    github_release_manifest_t manifest;
    esp_err_t manifest_err = github_ota_get_release_manifest(&manifest);
    if (manifest_err == ESP_OK && strlen(manifest.webui.version) > 0) {
        char current_webui_ver[16] = {0};
        webui_ota_get_current_version(current_webui_ver, sizeof(current_webui_ver));

        int webui_cmp = github_ota_compare_webui_versions(manifest.webui.version, current_webui_ver);

        // Check if recovery is needed or update is available
        bool needs_recovery = !webui_ota_is_partition_healthy();

        if (needs_recovery) {
            ESP_LOGW(TAG, "Web UI recovery needed, downloading latest version");
            // Check failure circuit breaker
            webui_ota_status_t webui_status;
            webui_ota_get_status(&webui_status);
            if (webui_status.failure_count > WEBUI_OTA_MAX_FAILURES) {
                ESP_LOGW(TAG, "Web UI OTA disabled due to too many failures (%d)", webui_status.failure_count);
            } else if (strlen(manifest.webui.download_url) > 0) {
                webui_ota_install_update(manifest.webui.download_url, manifest.webui.sha256, NULL);
            }
        } else if (webui_cmp > 0) {
            ESP_LOGI(TAG, "Web UI update available: %s -> %s", current_webui_ver, manifest.webui.version);
            // Check failure circuit breaker
            webui_ota_status_t webui_status;
            webui_ota_get_status(&webui_status);
            if (webui_status.failure_count > WEBUI_OTA_MAX_FAILURES) {
                ESP_LOGW(TAG, "Web UI OTA disabled due to too many failures (%d)", webui_status.failure_count);
            } else if (strlen(manifest.webui.download_url) > 0) {
                webui_ota_install_update(manifest.webui.download_url, manifest.webui.sha256, NULL);
            }
        } else {
            ESP_LOGI(TAG, "Web UI is up to date (current: %s, latest: %s)",
                     current_webui_ver, manifest.webui.version);
        }
    }
#endif

    s_ota.check_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ota_manager_check_for_update(void)
{
    if (!s_ota.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ota.state == OTA_STATE_CHECKING) {
        ESP_LOGW(TAG, "Check already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Clear previous error
    s_ota.error_message[0] = '\0';

    // Start check task with SPIRAM-backed stack
    const size_t ota_check_stack_size = 8192;
    if (!s_ota_check_stack) {
        s_ota_check_stack = heap_caps_malloc(ota_check_stack_size * sizeof(StackType_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    bool task_created = false;
    if (s_ota_check_stack) {
        s_ota.check_task = xTaskCreateStatic(ota_check_task, "ota_check",
                                              ota_check_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY,
                                              s_ota_check_stack, &s_ota_check_task_buffer);
        task_created = (s_ota.check_task != NULL);
    }

    if (!task_created) {
        if (xTaskCreate(ota_check_task, "ota_check",
                        ota_check_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_ota.check_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create check task");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
