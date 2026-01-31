// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file ota_manager.c
 * @brief OTA Manager implementation
 */

#include "ota_manager.h"
#include "github_ota.h"
#include "sdio_bus.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ota_manager";

// PSRAM-backed stack for OTA check task
static StackType_t *s_ota_check_stack = NULL;
static StaticTask_t s_ota_check_task_buffer;

// Check interval in microseconds (hours to us)
#define CHECK_INTERVAL_US (CONFIG_OTA_CHECK_INTERVAL_HOURS * 60ULL * 60ULL * 1000000ULL)

// Web UI OTA max consecutive failures before disabling auto-update
#define WEBUI_OTA_MAX_FAILURES 4

// OTA state
static struct {
    ota_state_t state;
    github_release_info_t release_info;
    int64_t last_check_time;
    int download_progress;
    char error_message[128];
    SemaphoreHandle_t mutex;
    esp_timer_handle_t check_timer;
    TaskHandle_t check_task;
    ota_progress_cb_t progress_callback;
    ota_ui_cb_t ui_callback;
    bool initialized;
    bool ui_active;
} s_ota = {
    .state = OTA_STATE_IDLE,
    .initialized = false,
    .ui_active = false
};

#if CONFIG_OTA_WEBUI_ENABLE
// Web UI OTA state (struct defined here, used in ota_manager_init)
static struct {
    bool partition_valid;
    bool needs_recovery;
    uint8_t failure_count;
    char current_version[16];
    char available_version[16];
    char available_url[256];
    char available_sha256[65];
    bool update_available;
    SemaphoreHandle_t mutex;
    TaskHandle_t install_task;
    webui_ota_state_t state;
    int progress;
    char status_message[64];
    char error_message[128];
} s_webui_ota = {
    .partition_valid = true,
    .needs_recovery = false,
    .failure_count = 0,
    .update_available = false,
    .state = WEBUI_OTA_STATE_IDLE,
    .progress = 0,
};
#endif

// Forward declarations
static void ota_check_task(void *arg);
static void ota_timer_callback(void *arg);
static esp_err_t ota_check_wifi_connected(void);
static esp_err_t ota_verify_partition_sha256(const esp_partition_t *partition, 
                                              size_t size, 
                                              const uint8_t expected[32]);

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

static void set_state(ota_state_t new_state)
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

static void set_error(const char *message)
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

static void set_progress(int percent, const char *status)
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

static void ota_exit_ui_mode(void)
{
    if (s_ota.ui_active && s_ota.ui_callback) {
        s_ota.ui_callback(false, NULL, NULL);
        s_ota.ui_active = false;
    }
}

// External functions we'll call (defined elsewhere in p3a)
extern bool animation_player_is_sd_export_locked(void);
extern bool animation_player_is_loader_busy(void);
extern bool animation_player_is_ui_mode(void);
extern void animation_player_pause_sd_access(void);
extern void animation_player_resume_sd_access(void);

#if CONFIG_P3A_PICO8_ENABLE
extern bool pico8_stream_is_active(void);
#else
static inline bool pico8_stream_is_active(void) { return false; }
#endif

// Retry parameters for OTA check when animation loader is busy
#define OTA_CHECK_RETRY_DELAY_MS    5000
#define OTA_CHECK_MAX_RETRIES       6

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

static esp_err_t ota_check_wifi_connected(void)
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

#if CONFIG_OTA_WEBUI_ENABLE
    // Initialize web UI OTA mutex
    s_webui_ota.mutex = xSemaphoreCreateMutex();
    if (!s_webui_ota.mutex) {
        ESP_LOGE(TAG, "Failed to create webui mutex");
        vSemaphoreDelete(s_ota.mutex);
        s_ota.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
#endif

    // Create periodic check timer
    esp_timer_create_args_t timer_args = {
        .callback = ota_timer_callback,
        .name = "ota_check",
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &s_ota.check_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_ota.mutex);
        s_ota.mutex = NULL;
        return err;
    }
    
    // Start periodic timer
    err = esp_timer_start_periodic(s_ota.check_timer, CHECK_INTERVAL_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_ota.check_timer);
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

static esp_err_t ota_verify_partition_sha256(const esp_partition_t *partition, 
                                              size_t size, 
                                              const uint8_t expected[32])
{
    if (!partition || !expected || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Verifying SHA256 of partition %s (%zu bytes)...", partition->label, size);
    
    mbedtls_sha256_context ctx;
    uint8_t computed[32];
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    
    if (!buf) {
        buf = malloc(4096);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = (size - offset < 4096) ? (size - offset) : 4096;
        esp_err_t err = esp_partition_read(partition, offset, buf, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Partition read failed at offset %zu: %s", offset, esp_err_to_name(err));
            free(buf);
            mbedtls_sha256_free(&ctx);
            return err;
        }
        mbedtls_sha256_update(&ctx, buf, chunk);
        offset += chunk;
        
        // Update progress callback during verification
        if ((offset % (256 * 1024) == 0 || offset >= size)) {
            int verify_progress = (int)((offset * 100) / size);
            if (s_ota.progress_callback) {
                s_ota.progress_callback(verify_progress, "Verifying checksum...");
            }
        }
    }
    
    mbedtls_sha256_finish(&ctx, computed);
    mbedtls_sha256_free(&ctx);
    free(buf);
    
    if (memcmp(computed, expected, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch!");
        ESP_LOG_BUFFER_HEX_LEVEL("expected", expected, 32, ESP_LOG_ERROR);
        ESP_LOG_BUFFER_HEX_LEVEL("computed", computed, 32, ESP_LOG_ERROR);
        return ESP_ERR_INVALID_CRC;
    }
    
    ESP_LOGI(TAG, "SHA256 verification passed");
    return ESP_OK;
}

esp_err_t ota_manager_install_update(ota_progress_cb_t progress_cb, ota_ui_cb_t ui_cb)
{
    if (!s_ota.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ota.state != OTA_STATE_UPDATE_AVAILABLE) {
        ESP_LOGE(TAG, "No update available (state=%s)", ota_state_to_string(s_ota.state));
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check blockers
    const char *block_reason;
    if (ota_manager_is_blocked(&block_reason)) {
        ESP_LOGE(TAG, "OTA blocked: %s", block_reason);
        set_error(block_reason);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check WiFi
    if (ota_check_wifi_connected() != ESP_OK) {
        set_error("No WiFi connection");
        return ESP_ERR_NOT_FOUND;
    }
    
    s_ota.progress_callback = progress_cb;
    s_ota.ui_callback = ui_cb;
    s_ota.download_progress = 0;
    
    const esp_app_desc_t *current_app = esp_app_get_description();
    ESP_LOGI(TAG, "Starting OTA update: %s -> %s", 
             current_app->version, s_ota.release_info.version);
    
    // Enter unified p3a OTA state
    esp_err_t state_err = p3a_state_enter_ota();
    if (state_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enter p3a OTA state: %s (continuing anyway)", esp_err_to_name(state_err));
    }
    
    // Update render state with version info
    p3a_render_set_ota_progress(0, "Preparing...", current_app->version, s_ota.release_info.version);
    
    // Enter UI mode to stop animations and free memory
    if (ui_cb) {
        ui_cb(true, current_app->version, s_ota.release_info.version);
        s_ota.ui_active = true;
    }
    
    set_progress(0, "Preparing...");
    
    // Wait for system to stabilize after UI mode transition
    ESP_LOGI(TAG, "Waiting for network to stabilize after UI mode transition...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Re-check WiFi after UI mode transition
    if (ota_check_wifi_connected() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi disconnected after UI mode transition");
        set_error("WiFi disconnected");
        set_progress(0, "WIFI ERROR!");
        vTaskDelay(pdMS_TO_TICKS(5000));
        ota_exit_ui_mode();
        set_state(OTA_STATE_ERROR);
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return ESP_ERR_NOT_FOUND;
    }
    
    // Download SHA256 first if available
    uint8_t expected_sha256[32] = {0};
    bool have_sha256 = false;
    bool sha256_required = strlen(s_ota.release_info.sha256_url) > 0;
    
    if (sha256_required) {
        set_progress(0, "Downloading checksum...");
        
        char sha256_hex[65];
        esp_err_t err = ESP_FAIL;
        
        // Retry SHA256 download up to 3 times with delays
        for (int retry = 0; retry < 3 && err != ESP_OK; retry++) {
            if (retry > 0) {
                ESP_LOGW(TAG, "Retrying SHA256 download (attempt %d/3)...", retry + 1);
                vTaskDelay(pdMS_TO_TICKS(2000));  // Wait before retry
            }
            err = github_ota_download_sha256(s_ota.release_info.sha256_url, sha256_hex, sizeof(sha256_hex));
        }
        
        if (err == ESP_OK) {
            err = github_ota_hex_to_bin(sha256_hex, expected_sha256);
            if (err == ESP_OK) {
                have_sha256 = true;
                ESP_LOGI(TAG, "SHA256 checksum downloaded successfully");
            } else {
                ESP_LOGE(TAG, "Failed to parse SHA256 hex string");
            }
        } else {
            ESP_LOGE(TAG, "Failed to download SHA256 checksum after 3 attempts");
        }
        
        if (!have_sha256) {
            // SHA256 was expected but failed - refuse to proceed
            ESP_LOGE(TAG, "Cannot verify firmware integrity - aborting update");
            set_error("Checksum download failed");
            set_progress(0, "CHECKSUM ERROR!");
            vTaskDelay(pdMS_TO_TICKS(5000));  // Show error for 5 seconds
            ota_exit_ui_mode();
            set_state(OTA_STATE_ERROR);
            // Exit p3a OTA state and return to playback
            p3a_state_exit_to_playback();
            return ESP_ERR_INVALID_CRC;
        }
    } else {
        ESP_LOGW(TAG, "No SHA256 URL provided, proceeding without checksum verification");
    }
    
    // Configure OTA
    set_state(OTA_STATE_DOWNLOADING);
    set_progress(0, "Connecting to server...");
    
    esp_http_client_config_t http_config = {
        .url = s_ota.release_info.download_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_OTA_DOWNLOAD_TIMEOUT_SEC * 1000,
        .keep_alive_enable = true,
        .buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,  // GitHub redirects to CDN
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partial_http_download = false,
    };
    
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_error("Failed to start download");
        return err;
    }
    
    // Get image info
    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get image description: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        set_error("Invalid firmware image");
        return err;
    }
    
    ESP_LOGI(TAG, "New firmware: version=%s, project=%s", new_app_info.version, new_app_info.project_name);
    
    // Download with progress
    int total_size = esp_https_ota_get_image_size(ota_handle);
    ESP_LOGI(TAG, "Downloading %d bytes...", total_size);
    
    set_progress(0, "Downloading firmware...");
    
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        int downloaded = esp_https_ota_get_image_len_read(ota_handle);
        int percent = (total_size > 0) ? (downloaded * 100 / total_size) : 0;
        set_progress(percent, "Downloading...");
        // Update render state with version info
        p3a_render_set_ota_progress(percent, "Downloading...", current_app->version, s_ota.release_info.version);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        set_error("Download failed");
        ota_exit_ui_mode();
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return err;
    }
    
    // Check if image is valid
    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(ota_handle);
        set_error("Incomplete download");
        set_progress(s_ota.download_progress, "DOWNLOAD ERROR!");
        vTaskDelay(pdMS_TO_TICKS(2000));
        ota_exit_ui_mode();
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return ESP_ERR_INVALID_SIZE;
    }
    
    set_state(OTA_STATE_FLASHING);
    set_progress(100, "Writing to flash...");
    
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        set_progress(100, "FLASH ERROR!");
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            set_error("Image validation failed");
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
            set_error("Flash write failed");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        ota_exit_ui_mode();
        // Exit p3a OTA state and return to playback
        p3a_state_exit_to_playback();
        return err;
    }
    
    // Verify SHA256 if we have it
    if (have_sha256) {
        set_state(OTA_STATE_VERIFYING);
        set_progress(0, "Verifying checksum...");
        
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition) {
            err = ota_verify_partition_sha256(update_partition, total_size, expected_sha256);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "SHA256 verification failed!");
                set_progress(100, "VERIFY ERROR!");
                vTaskDelay(pdMS_TO_TICKS(2000));
                ota_exit_ui_mode();
                // Mark partition as invalid
                set_error("Checksum verification failed");
                // Exit p3a OTA state and return to playback
                p3a_state_exit_to_playback();
                return err;
            }
        }
    }
    
    set_state(OTA_STATE_PENDING_REBOOT);
    set_progress(100, "Update complete!");
    
    ESP_LOGI(TAG, "OTA update successful! Rebooting in 3 seconds...");
    
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // Note: We don't exit UI mode since we're rebooting immediately
    esp_restart();
    
    // Never reached
    return ESP_OK;
}

esp_err_t ota_manager_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);
    
    if (!other) {
        ESP_LOGE(TAG, "No rollback partition available");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Check if there's a valid image in the other slot
    esp_app_desc_t other_app_info;
    esp_err_t err = esp_ota_get_partition_description(other, &other_app_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No valid image in rollback partition");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Rolling back from %s to %s", 
             esp_app_get_description()->version, other_app_info.version);
    
    err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Rollback scheduled, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    // Never reached
    return ESP_OK;
}

esp_err_t ota_manager_validate_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGW(TAG, "Could not get running partition");
        return ESP_OK;  // Not an error, might be factory partition
    }
    
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err != ESP_OK) {
        // Probably running from factory partition
        ESP_LOGI(TAG, "Running from non-OTA partition");
        return ESP_OK;
    }
    
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New OTA firmware pending verification");
        
        // Run basic self-tests
        // For now, just check that we got this far successfully
        // In a production system, you might check LCD, WiFi, etc.
        
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            return err;
        }
        
        ESP_LOGI(TAG, "OTA firmware validated successfully");
    } else if (ota_state == ESP_OTA_IMG_VALID) {
        ESP_LOGD(TAG, "Running validated OTA firmware");
    }

    return ESP_OK;
}

// =============================================================================
// Web UI OTA Implementation
// =============================================================================

#if CONFIG_OTA_WEBUI_ENABLE

#include "esp_littlefs.h"

// NVS keys for web UI OTA state
#define NVS_WEBUI_PARTITION_INVALID "webui_invalid"
#define NVS_WEBUI_NEEDS_RECOVERY    "webui_recover"
#define NVS_WEBUI_OTA_FAILURES      "webui_failures"

const char *webui_ota_state_to_string(webui_ota_state_t state)
{
    switch (state) {
        case WEBUI_OTA_STATE_IDLE:        return "idle";
        case WEBUI_OTA_STATE_DOWNLOADING: return "downloading";
        case WEBUI_OTA_STATE_UNMOUNTING:  return "unmounting";
        case WEBUI_OTA_STATE_ERASING:     return "erasing";
        case WEBUI_OTA_STATE_WRITING:     return "writing";
        case WEBUI_OTA_STATE_VERIFYING:   return "verifying";
        case WEBUI_OTA_STATE_REMOUNTING:  return "remounting";
        case WEBUI_OTA_STATE_COMPLETE:    return "complete";
        case WEBUI_OTA_STATE_ERROR:       return "error";
        default:                          return "unknown";
    }
}

static void webui_set_state(webui_ota_state_t new_state, const char *status_message)
{
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }
    s_webui_ota.state = new_state;
    if (status_message) {
        strncpy(s_webui_ota.status_message, status_message, sizeof(s_webui_ota.status_message) - 1);
        s_webui_ota.status_message[sizeof(s_webui_ota.status_message) - 1] = '\0';
    }
    ESP_LOGI(TAG, "WebUI OTA state: %s (%s)", webui_ota_state_to_string(new_state),
             status_message ? status_message : "");
    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }
}

static void webui_set_progress(int percent, const char *status_message)
{
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }
    s_webui_ota.progress = percent;
    if (status_message) {
        strncpy(s_webui_ota.status_message, status_message, sizeof(s_webui_ota.status_message) - 1);
        s_webui_ota.status_message[sizeof(s_webui_ota.status_message) - 1] = '\0';
    }
    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }
}

static void webui_set_error(const char *error_message)
{
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }
    s_webui_ota.state = WEBUI_OTA_STATE_ERROR;
    if (error_message) {
        strncpy(s_webui_ota.error_message, error_message, sizeof(s_webui_ota.error_message) - 1);
        s_webui_ota.error_message[sizeof(s_webui_ota.error_message) - 1] = '\0';
        strncpy(s_webui_ota.status_message, error_message, sizeof(s_webui_ota.status_message) - 1);
        s_webui_ota.status_message[sizeof(s_webui_ota.status_message) - 1] = '\0';
    }
    ESP_LOGE(TAG, "WebUI OTA error: %s", error_message ? error_message : "Unknown error");
    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }
}

static esp_err_t webui_ota_read_nvs_flags(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        // NVS namespace doesn't exist yet, use defaults
        return ESP_OK;
    }

    uint8_t invalid = 0;
    if (nvs_get_u8(nvs, NVS_WEBUI_PARTITION_INVALID, &invalid) == ESP_OK) {
        s_webui_ota.partition_valid = (invalid == 0);
    }

    uint8_t recover = 0;
    if (nvs_get_u8(nvs, NVS_WEBUI_NEEDS_RECOVERY, &recover) == ESP_OK) {
        s_webui_ota.needs_recovery = (recover != 0);
    }

    uint8_t failures = 0;
    if (nvs_get_u8(nvs, NVS_WEBUI_OTA_FAILURES, &failures) == ESP_OK) {
        s_webui_ota.failure_count = failures;
    }

    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t webui_ota_set_nvs_flag(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

esp_err_t webui_ota_get_current_version(char *version, size_t buf_size)
{
    if (!version || buf_size < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen("/spiffs/version.txt", "r");
    if (!f) {
        // Only log warning if partition is supposed to be valid
        // (avoids noise during OTA updates when filesystem is temporarily unmounted)
        if (s_webui_ota.partition_valid) {
            ESP_LOGW(TAG, "Web UI version.txt not found");
        }
        return ESP_ERR_NOT_FOUND;
    }

    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(f);

    // Trim whitespace
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) {
        buf[--len] = '\0';
    }

    strncpy(version, buf, buf_size - 1);
    version[buf_size - 1] = '\0';

    return ESP_OK;
}

bool webui_ota_is_partition_healthy(void)
{
    // Read NVS flags
    webui_ota_read_nvs_flags();

    // Check if partition was marked invalid
    if (!s_webui_ota.partition_valid) {
        ESP_LOGW(TAG, "Web UI partition marked invalid in NVS");
        return false;
    }

    // Check if version.txt exists and is readable
    char version[16];
    if (webui_ota_get_current_version(version, sizeof(version)) != ESP_OK) {
        ESP_LOGW(TAG, "Web UI version.txt not readable");
        return false;
    }

    // Store current version
    snprintf(s_webui_ota.current_version, sizeof(s_webui_ota.current_version), "%s", version);

    ESP_LOGI(TAG, "Web UI partition healthy, version: %s", version);
    return true;
}

void webui_ota_set_needs_recovery(void)
{
    s_webui_ota.needs_recovery = true;
    webui_ota_set_nvs_flag(NVS_WEBUI_NEEDS_RECOVERY, 1);
    ESP_LOGW(TAG, "Web UI recovery flagged");
}

esp_err_t webui_ota_get_status(webui_ota_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(webui_ota_status_t));

    // Thread-safe read of state
    if (s_webui_ota.mutex) {
        xSemaphoreTake(s_webui_ota.mutex, portMAX_DELAY);
    }

    // Read current version
    webui_ota_get_current_version(status->current_version, sizeof(status->current_version));

    // Copy cached state
    snprintf(status->available_version, sizeof(status->available_version), "%s", s_webui_ota.available_version);
    status->update_available = s_webui_ota.update_available;
    status->partition_valid = s_webui_ota.partition_valid;
    status->needs_recovery = s_webui_ota.needs_recovery;
    status->failure_count = s_webui_ota.failure_count;
    status->auto_update_disabled = (s_webui_ota.failure_count > WEBUI_OTA_MAX_FAILURES);

    // Copy new state/progress fields
    status->state = s_webui_ota.state;
    status->progress = s_webui_ota.progress;
    snprintf(status->status_message, sizeof(status->status_message), "%s", s_webui_ota.status_message);
    snprintf(status->error_message, sizeof(status->error_message), "%s", s_webui_ota.error_message);

    if (s_webui_ota.mutex) {
        xSemaphoreGive(s_webui_ota.mutex);
    }

    return ESP_OK;
}

// Context for web UI download event handler
typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    size_t received;
    ota_progress_cb_t progress_cb;
    int content_length;
} webui_download_ctx_t;

static esp_err_t webui_http_event_handler(esp_http_client_event_t *evt)
{
    webui_download_ctx_t *ctx = (webui_download_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // Capture content-length for progress reporting
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                ctx->content_length = atoi(evt->header_value);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (ctx && ctx->buffer && evt->data_len > 0) {
                size_t remaining = ctx->buffer_size - ctx->received;
                size_t to_copy = (evt->data_len < remaining) ? evt->data_len : remaining;
                if (to_copy > 0) {
                    memcpy(ctx->buffer + ctx->received, evt->data, to_copy);
                    ctx->received += to_copy;

                    // Report and persist progress
                    if (ctx->content_length > 0) {
                        int percent = (int)((ctx->received * 100) / ctx->content_length);
                        webui_set_progress(percent, "Downloading web UI...");
                        if (ctx->progress_cb) {
                            ctx->progress_cb(percent, "Downloading web UI...");
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t webui_ota_download_and_verify(const char *url,
                                                const char *expected_sha256,
                                                uint8_t **out_data,
                                                size_t *out_size,
                                                ota_progress_cb_t progress_cb)
{
    ESP_LOGI(TAG, "Downloading web UI from: %s", url);

    // Allocate buffer for storage.bin (up to 4MB)
    // Use PSRAM for the download buffer
    size_t max_size = 4 * 1024 * 1024;
    uint8_t *buffer = heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        return ESP_ERR_NO_MEM;
    }

    // Download context for event handler
    webui_download_ctx_t ctx = {
        .buffer = buffer,
        .buffer_size = max_size,
        .received = 0,
        .progress_cb = progress_cb,
        .content_length = 0,
    };

    // Use esp_http_client_perform() which properly handles GitHub redirects to CDN
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_OTA_DOWNLOAD_TIMEOUT_SEC * 1000,
        .buffer_size = CONFIG_OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,  // GitHub redirects to CDN
        .event_handler = webui_http_event_handler,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(buffer);
        return err;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status_code);
        free(buffer);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    size_t total_read = ctx.received;
    ESP_LOGI(TAG, "Downloaded %zu bytes", total_read);

    // Verify SHA256
    if (expected_sha256 && strlen(expected_sha256) == 64) {
        if (progress_cb) {
            progress_cb(100, "Verifying checksum...");
        }

        mbedtls_sha256_context sha_ctx;
        uint8_t computed[32];

        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
        mbedtls_sha256_update(&sha_ctx, buffer, total_read);
        mbedtls_sha256_finish(&sha_ctx, computed);
        mbedtls_sha256_free(&sha_ctx);

        // Convert expected to binary
        uint8_t expected[32];
        if (github_ota_hex_to_bin(expected_sha256, expected) != ESP_OK) {
            ESP_LOGE(TAG, "Invalid SHA256 hex string");
            free(buffer);
            return ESP_ERR_INVALID_ARG;
        }

        if (memcmp(computed, expected, 32) != 0) {
            ESP_LOGE(TAG, "SHA256 mismatch!");
            free(buffer);
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGI(TAG, "SHA256 verification passed");
    } else {
        ESP_LOGW(TAG, "No SHA256 provided, skipping verification");
    }

    *out_data = buffer;
    *out_size = total_read;
    return ESP_OK;
}

esp_err_t webui_ota_install_update(const char *download_url,
                                    const char *expected_sha256,
                                    ota_progress_cb_t progress_cb)
{
    if (!download_url || strlen(download_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting web UI OTA update...");

    // Initialize state
    webui_set_state(WEBUI_OTA_STATE_DOWNLOADING, "Starting download...");
    webui_set_progress(0, "Starting download...");
    s_webui_ota.error_message[0] = '\0';

    // Increment failure counter BEFORE starting (defensive)
    s_webui_ota.failure_count++;
    webui_ota_set_nvs_flag(NVS_WEBUI_OTA_FAILURES, s_webui_ota.failure_count);

    // Download and verify
    uint8_t *data = NULL;
    size_t data_size = 0;
    esp_err_t err = webui_ota_download_and_verify(download_url, expected_sha256,
                                                   &data, &data_size, progress_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download/verify failed: %s", esp_err_to_name(err));
        webui_set_error("Download failed");
        return err;
    }

    // Set partition invalid flag BEFORE modifying partition
    webui_ota_set_nvs_flag(NVS_WEBUI_PARTITION_INVALID, 1);
    s_webui_ota.partition_valid = false;

    webui_set_state(WEBUI_OTA_STATE_UNMOUNTING, "Unmounting filesystem...");
    webui_set_progress(0, "Unmounting filesystem...");
    if (progress_cb) {
        progress_cb(0, "Unmounting filesystem...");
    }

    // Unmount LittleFS
    esp_err_t unmount_err = esp_vfs_littlefs_unregister("storage");
    if (unmount_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to unmount LittleFS: %s (continuing anyway)",
                 esp_err_to_name(unmount_err));
    }

    // Get storage partition
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!partition) {
        ESP_LOGE(TAG, "Storage partition not found");
        free(data);
        webui_set_error("Storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Storage partition: offset=0x%lx, size=%lu",
             partition->address, partition->size);

    if (data_size > partition->size) {
        ESP_LOGE(TAG, "Image too large: %zu > %lu", data_size, partition->size);
        free(data);
        webui_set_error("Image too large");
        return ESP_ERR_INVALID_SIZE;
    }

    webui_set_state(WEBUI_OTA_STATE_ERASING, "Erasing partition...");
    webui_set_progress(0, "Erasing partition...");
    if (progress_cb) {
        progress_cb(0, "Erasing partition...");
    }

    // Erase partition
    err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        free(data);
        webui_set_error("Partition erase failed");
        return err;
    }

    webui_set_state(WEBUI_OTA_STATE_WRITING, "Writing to flash...");
    webui_set_progress(0, "Writing to flash...");
    if (progress_cb) {
        progress_cb(0, "Writing to flash...");
    }

    // Write data to partition
    size_t offset = 0;
    size_t chunk_size = 4096;
    while (offset < data_size) {
        size_t to_write = (data_size - offset < chunk_size) ? (data_size - offset) : chunk_size;
        err = esp_partition_write(partition, offset, data + offset, to_write);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed at offset %zu: %s", offset, esp_err_to_name(err));
            free(data);
            webui_set_error("Flash write failed");
            return err;
        }
        offset += to_write;

        int percent = (int)((offset * 100) / data_size);
        webui_set_progress(percent, "Writing to flash...");
        if (progress_cb) {
            progress_cb(percent, "Writing to flash...");
        }
    }

    free(data);

    webui_set_state(WEBUI_OTA_STATE_VERIFYING, "Verifying write...");
    webui_set_progress(0, "Verifying write...");
    if (progress_cb) {
        progress_cb(100, "Verifying write...");
    }

    // Post-write verification: read back and verify SHA256
    if (expected_sha256 && strlen(expected_sha256) == 64) {
        mbedtls_sha256_context ctx;
        uint8_t computed[32];
        uint8_t *read_buf = malloc(4096);

        if (!read_buf) {
            ESP_LOGE(TAG, "Failed to allocate verification buffer");
            webui_set_error("Out of memory");
            return ESP_ERR_NO_MEM;
        }

        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);

        offset = 0;
        while (offset < data_size) {
            size_t to_read = (data_size - offset < 4096) ? (data_size - offset) : 4096;
            err = esp_partition_read(partition, offset, read_buf, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Verification read failed at offset %zu", offset);
                free(read_buf);
                mbedtls_sha256_free(&ctx);
                webui_set_error("Verification read failed");
                return err;
            }
            mbedtls_sha256_update(&ctx, read_buf, to_read);
            offset += to_read;

            int percent = (int)((offset * 100) / data_size);
            webui_set_progress(percent, "Verifying write...");
        }

        mbedtls_sha256_finish(&ctx, computed);
        mbedtls_sha256_free(&ctx);
        free(read_buf);

        uint8_t expected[32];
        github_ota_hex_to_bin(expected_sha256, expected);

        if (memcmp(computed, expected, 32) != 0) {
            ESP_LOGE(TAG, "Post-write SHA256 verification failed!");
            webui_set_error("Checksum verification failed");
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGI(TAG, "Post-write verification passed");
    }

    webui_set_state(WEBUI_OTA_STATE_REMOUNTING, "Remounting filesystem...");
    webui_set_progress(100, "Remounting filesystem...");
    if (progress_cb) {
        progress_cb(100, "Remounting filesystem...");
    }

    // Remount LittleFS
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };

    err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remount LittleFS: %s", esp_err_to_name(err));
        webui_set_error("Failed to remount filesystem");
        return err;
    }

    // Verify version.txt is readable
    char new_version[16] = {0};
    err = webui_ota_get_current_version(new_version, sizeof(new_version));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read version.txt after update");
        webui_set_error("Failed to verify update");
        return err;
    }

    ESP_LOGI(TAG, "Web UI updated successfully to version %s", new_version);

    // Clear all failure flags - update succeeded!
    webui_ota_set_nvs_flag(NVS_WEBUI_PARTITION_INVALID, 0);
    webui_ota_set_nvs_flag(NVS_WEBUI_NEEDS_RECOVERY, 0);
    webui_ota_set_nvs_flag(NVS_WEBUI_OTA_FAILURES, 0);

    s_webui_ota.partition_valid = true;
    s_webui_ota.needs_recovery = false;
    s_webui_ota.failure_count = 0;
    s_webui_ota.update_available = false;
    snprintf(s_webui_ota.current_version, sizeof(s_webui_ota.current_version), "%s", new_version);

    webui_set_state(WEBUI_OTA_STATE_COMPLETE, "Update complete!");
    webui_set_progress(100, "Update complete!");
    if (progress_cb) {
        progress_cb(100, "Web UI update complete!");
    }

    // After a short delay, return to idle
    vTaskDelay(pdMS_TO_TICKS(3000));
    webui_set_state(WEBUI_OTA_STATE_IDLE, "");
    s_webui_ota.progress = 0;
    s_webui_ota.status_message[0] = '\0';

    return ESP_OK;
}

static void webui_ota_repair_task(void *arg)
{
    ESP_LOGI(TAG, "Web UI repair task started");

    // Get the latest manifest to find the download URL
    github_release_manifest_t manifest;
    esp_err_t err = github_ota_get_release_manifest(&manifest);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get release manifest for repair");
        s_webui_ota.install_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (strlen(manifest.webui.download_url) == 0) {
        ESP_LOGE(TAG, "No web UI download URL in manifest");
        s_webui_ota.install_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Install the update
    err = webui_ota_install_update(manifest.webui.download_url,
                                    manifest.webui.sha256, NULL);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web UI repair failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Web UI repair completed successfully");
    }

    s_webui_ota.install_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t webui_ota_trigger_repair(void)
{
    if (s_webui_ota.install_task != NULL) {
        ESP_LOGW(TAG, "Web UI repair already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Bypass failure counter for manual repairs
    BaseType_t ret = xTaskCreate(webui_ota_repair_task, "webui_repair",
                                  8192, NULL, 5, &s_webui_ota.install_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create repair task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else // !CONFIG_OTA_WEBUI_ENABLE

// Stub implementations when web UI OTA is disabled

esp_err_t webui_ota_get_current_version(char *version, size_t buf_size)
{
    if (version && buf_size > 0) {
        version[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t webui_ota_get_status(webui_ota_status_t *status)
{
    if (status) {
        memset(status, 0, sizeof(webui_ota_status_t));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool webui_ota_is_partition_healthy(void)
{
    return true;  // Assume healthy when OTA is disabled
}

void webui_ota_set_needs_recovery(void)
{
    // No-op
}

esp_err_t webui_ota_trigger_repair(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t webui_ota_install_update(const char *download_url,
                                    const char *expected_sha256,
                                    ota_progress_cb_t progress_cb)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_OTA_WEBUI_ENABLE

