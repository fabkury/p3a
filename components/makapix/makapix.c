#include "makapix.h"
#include "makapix_store.h"
#include "makapix_provision.h"
#include "makapix_mqtt.h"
#include "makapix_api.h"
#include "makapix_channel_impl.h"
#include "makapix_artwork.h"
#include "download_manager.h"
#include "sdio_bus.h"
#include "app_wifi.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include "esp_err.h"
#include "esp_log.h"

// Forward declarations (to avoid including headers with dependencies not available to this component)
esp_err_t app_lcd_enter_ui_mode(void);
void app_lcd_exit_ui_mode(void);
esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent);
void ugfx_ui_hide_channel_message(void);
esp_err_t channel_player_switch_to_makapix_channel(channel_handle_t makapix_channel);
esp_err_t channel_player_switch_to_sdcard_channel(void);
esp_err_t animation_player_request_swap_current(void);
void channel_player_clear_channel(channel_handle_t channel_to_clear);
// channel_request_refresh is a macro defined in channel_interface.h - no forward declaration needed
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "makapix";

static makapix_state_t s_state = MAKAPIX_STATE_IDLE;
static int32_t s_current_post_id = 0;
static bool s_view_intent_intentional = false;  // Track if next view should be intentional
static char s_registration_code[8] = {0};  // 6 chars + null + padding to avoid warning
static char s_registration_expires[64] = {0};  // Extra space to avoid truncation warning
static char s_provisioning_status[128] = {0};  // Status message during provisioning
static TimerHandle_t s_status_timer = NULL;
static bool s_provisioning_cancelled = false;  // Flag to prevent race condition
static TaskHandle_t s_poll_task_handle = NULL;  // Handle for credential polling task
static TaskHandle_t s_reconnect_task_handle = NULL;  // Handle for MQTT reconnection task
static TaskHandle_t s_status_publish_task_handle = NULL;  // Handle for status publish task
static TaskHandle_t s_channel_switch_task_handle = NULL;  // Handle for channel switch task
static channel_handle_t s_current_channel = NULL;  // Current active Makapix channel

// Channel loading state tracking
static volatile bool s_channel_loading = false;          // True while a channel is being loaded
static volatile bool s_channel_load_abort = false;       // Signal to abort current load
static char s_loading_channel_id[128] = {0};             // Channel ID currently being loaded
static char s_current_channel_id[128] = {0};             // Channel ID currently active (for download cancellation)

// Pending channel request (set by handlers, processed by channel switch task)
static char s_pending_channel[64] = {0};                 // Requested channel name
static char s_pending_user_handle[64] = {0};             // User handle for by_user channel
static volatile bool s_has_pending_channel = false;      // True if a new channel was requested
static SemaphoreHandle_t s_channel_switch_sem = NULL;    // Semaphore to wake channel switch task

// Forward declarations
static void credentials_poll_task(void *pvParameters);
static void mqtt_reconnect_task(void *pvParameters);
static void status_publish_task(void *pvParameters);
static void channel_switch_task(void *pvParameters);
static channel_handle_t create_single_artwork_channel(const char *storage_key, const char *art_url);

#define STATUS_PUBLISH_INTERVAL_MS (30000) // 30 seconds

/**
 * @brief Timer callback for periodic status publishing
 * Lightweight callback that just notifies the dedicated task to do the work
 * Note: Timer callbacks run from the timer service task, not ISR context
 */
static void status_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    // Notify the dedicated task to publish status (non-blocking)
    if (s_status_publish_task_handle != NULL) {
        xTaskNotifyGive(s_status_publish_task_handle);
    }
}

/**
 * @brief Dedicated task for status publishing
 * Runs in its own context with sufficient stack for JSON operations and logging
 */
static void status_publish_task(void *pvParameters)
{
    (void)pvParameters;
    
    while (1) {
        // Wait for notification from timer callback
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Skip publishing if SDIO bus is locked (e.g., during OTA check/download)
        // MQTT publishing uses WiFi which could conflict with critical operations
        if (sdio_bus_is_locked()) {
            ESP_LOGD(TAG, "Skipping status publish: SDIO bus locked by %s",
                     sdio_bus_get_holder() ? sdio_bus_get_holder() : "unknown");
            continue;
        }
        
        // Publish status if MQTT is connected
        if (makapix_mqtt_is_connected()) {
            makapix_mqtt_publish_status(makapix_get_current_post_id());
        }
    }
}

/**
 * @brief Dedicated task for channel switching
 * Runs in its own context to avoid blocking HTTP/MQTT handlers
 * The blocking wait loop in makapix_switch_to_channel() runs here
 */
static void channel_switch_task(void *pvParameters)
{
    (void)pvParameters;
    char channel[64];
    char user_handle[64];
    
    ESP_LOGI(TAG, "Channel switch task started");
    
    while (1) {
        // Wait for signal that a channel switch is requested
        if (xSemaphoreTake(s_channel_switch_sem, portMAX_DELAY) == pdTRUE) {
            // Get pending channel info
            if (makapix_get_pending_channel(channel, sizeof(channel), user_handle, sizeof(user_handle))) {
                makapix_clear_pending_channel();
                
                const char *user = user_handle[0] ? user_handle : NULL;
                ESP_LOGI(TAG, "Channel switch task: switching to %s", channel);
                
                // This can block for up to 60 seconds - that's OK, we're in our own task
                esp_err_t err = makapix_switch_to_channel(channel, user);
                
                if (err == ESP_ERR_INVALID_STATE) {
                    // Channel load was aborted - a different channel was requested
                    // The pending channel mechanism will handle it on next iteration
                    ESP_LOGI(TAG, "Channel switch aborted for new request");
                } else if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Channel switch failed: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "Channel switch completed successfully");
                }
            }
        }
    }
}

/**
 * @brief MQTT connection state change callback
 */
static void mqtt_connection_callback(bool connected)
{
    ESP_LOGI(TAG, "=== MQTT CONNECTION CALLBACK ===");
    ESP_LOGI(TAG, "Connected: %s", connected ? "true" : "false");
    ESP_LOGI(TAG, "Previous state: %d", s_state);
    
    if (connected) {
        ESP_LOGI(TAG, "MQTT connected successfully");
        s_state = MAKAPIX_STATE_CONNECTED;
        ESP_LOGI(TAG, "New state: %d (CONNECTED)", s_state);
        
        // Publish initial status
        ESP_LOGI(TAG, "Publishing initial status...");
        makapix_mqtt_publish_status(makapix_get_current_post_id());

        // Create status publish task if it doesn't exist
        if (s_status_publish_task_handle == NULL) {
            ESP_LOGI(TAG, "Creating status publish task...");
            BaseType_t task_ret = xTaskCreate(status_publish_task, "status_pub", 4096, NULL, 5, &s_status_publish_task_handle);
            if (task_ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create status publish task");
                s_status_publish_task_handle = NULL;
            } else {
                ESP_LOGI(TAG, "Status publish task created successfully");
            }
        }

        // Create or restart periodic status timer
        if (!s_status_timer) {
            ESP_LOGI(TAG, "Creating status timer (interval: %d ms)", STATUS_PUBLISH_INTERVAL_MS);
            s_status_timer = xTimerCreate("status_timer", pdMS_TO_TICKS(STATUS_PUBLISH_INTERVAL_MS),
                                          pdTRUE, NULL, status_timer_callback);
            if (s_status_timer) {
                xTimerStart(s_status_timer, 0);
                ESP_LOGI(TAG, "Status timer created and started");
            } else {
                ESP_LOGE(TAG, "Failed to create status timer");
            }
        } else {
            // Timer exists but may have been stopped during disconnect - restart it
            xTimerStart(s_status_timer, 0);
            ESP_LOGI(TAG, "Status timer restarted");
        }
        
        // Trigger refresh on the current Makapix channel if one exists
        // This handles the boot-time case where the channel was loaded before MQTT connected
        if (s_current_channel && s_current_channel_id[0]) {
            ESP_LOGI(TAG, "Triggering refresh for current channel: %s", s_current_channel_id);
            channel_request_refresh(s_current_channel);
        }
    } else {
        ESP_LOGI(TAG, "MQTT disconnected");
        
        // Stop status timer to prevent publish attempts during reconnection
        if (s_status_timer) {
            xTimerStop(s_status_timer, 0);
            ESP_LOGI(TAG, "Status timer stopped");
        }
        
        // Delete status publish task to free resources
        if (s_status_publish_task_handle != NULL) {
            ESP_LOGI(TAG, "Deleting status publish task...");
            vTaskDelete(s_status_publish_task_handle);
            s_status_publish_task_handle = NULL;
            ESP_LOGI(TAG, "Status publish task deleted");
        }
        
        // Only transition to DISCONNECTED and start reconnection if we were connected
        // Don't interfere with provisioning or other states
        if (s_state == MAKAPIX_STATE_CONNECTED || s_state == MAKAPIX_STATE_CONNECTING) {
            s_state = MAKAPIX_STATE_DISCONNECTED;
            ESP_LOGI(TAG, "New state: %d (DISCONNECTED)", s_state);
            
            // Start reconnection task if not already running
            // Only do this when transitioning to DISCONNECTED (not during provisioning, etc.)
            if (s_reconnect_task_handle == NULL) {
                ESP_LOGI(TAG, "Starting reconnection task...");
                BaseType_t task_ret = xTaskCreate(mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle);
                if (task_ret != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create reconnection task");
                    s_reconnect_task_handle = NULL;
                } else {
                    ESP_LOGI(TAG, "Reconnection task created successfully");
                }
            } else {
                ESP_LOGI(TAG, "Reconnection task already running");
            }
        } else {
            ESP_LOGI(TAG, "State unchanged: %d (not starting reconnection)", s_state);
        }
    }
    ESP_LOGI(TAG, "=== END MQTT CONNECTION CALLBACK ===");
}

/**
 * @brief Provisioning task
 */
static void provisioning_task(void *pvParameters)
{
    makapix_provision_result_t result;
    
    // Set status to "Querying endpoint" right before making the HTTP request
    snprintf(s_provisioning_status, sizeof(s_provisioning_status), "Querying endpoint");
    
    esp_err_t err = makapix_provision_request(&result);

    // Check if provisioning was cancelled while we were waiting
    if (s_provisioning_cancelled) {
        ESP_LOGI(TAG, "Provisioning was cancelled, aborting");
        s_provisioning_cancelled = false;  // Reset flag
        vTaskDelete(NULL);
        return;
    }

    if (err == ESP_OK) {
        // Double-check cancellation before saving (race condition protection)
        if (s_provisioning_cancelled) {
            ESP_LOGI(TAG, "Provisioning was cancelled after request completed, aborting");
            s_provisioning_cancelled = false;
            vTaskDelete(NULL);
            return;
        }

        // Save credentials (player_key and broker info)
        // Note: Old registration data will be cleared later when credentials are successfully received
        err = makapix_store_save_credentials(result.player_key, result.mqtt_host, result.mqtt_port);
        if (err == ESP_OK) {
            // Final check before updating state
            if (!s_provisioning_cancelled) {
                // Store registration code for display
                snprintf(s_registration_code, sizeof(s_registration_code), "%s", result.registration_code);
                snprintf(s_registration_expires, sizeof(s_registration_expires), "%s", result.expires_at);
                
                s_state = MAKAPIX_STATE_SHOW_CODE;
                ESP_LOGI(TAG, "Provisioning successful, registration code: %s", s_registration_code);
                ESP_LOGI(TAG, "Starting credential polling task...");
                
                // Start credential polling task
                // Stack size needs to be large enough for makapix_credentials_result_t (3x 4096 byte arrays = ~12KB)
                BaseType_t poll_ret = xTaskCreate(credentials_poll_task, "cred_poll", 16384, NULL, 5, &s_poll_task_handle);
                if (poll_ret != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create credential polling task");
                    s_state = MAKAPIX_STATE_IDLE;
                    s_poll_task_handle = NULL;
                }
            } else {
                ESP_LOGI(TAG, "Provisioning was cancelled, discarding results");
                s_provisioning_cancelled = false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
            s_state = MAKAPIX_STATE_IDLE;
        }
    } else {
        ESP_LOGE(TAG, "Provisioning failed: %s", esp_err_to_name(err));
        if (!s_provisioning_cancelled) {
            s_state = MAKAPIX_STATE_IDLE;
        }
    }

    s_provisioning_cancelled = false;  // Reset flag
    vTaskDelete(NULL);
}

/**
 * @brief Credentials polling task
 * 
 * Polls for TLS certificates after registration code is displayed.
 * Runs while state is SHOW_CODE.
 */
static void credentials_poll_task(void *pvParameters)
{
    char player_key[37];
    
    // Get player_key from store
    if (makapix_store_get_player_key(player_key, sizeof(player_key)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get player_key for credential polling");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Starting credential polling for player_key: %s", player_key);

    makapix_credentials_result_t creds;
    int poll_count = 0;
    const int max_polls = 300; // 300 * 3 seconds = 15 minutes (registration code expiry)

    while (s_state == MAKAPIX_STATE_SHOW_CODE && poll_count < max_polls) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // Poll every 3 seconds
        
        if (s_provisioning_cancelled) {
            ESP_LOGI(TAG, "Provisioning cancelled, stopping credential polling");
            break;
        }

        poll_count++;
        ESP_LOGI(TAG, "Polling for credentials (attempt %d/%d)...", poll_count, max_polls);

        esp_err_t err = makapix_poll_credentials(player_key, &creds);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Credentials received! Saving to NVS...");
            
            // Preserve broker info before clearing (from initial provisioning response)
            // The credentials response may not include broker info, so we need to keep what we have
            char preserved_mqtt_host[64] = {0};
            uint16_t preserved_mqtt_port = 0;
            bool has_preserved_broker = false;
            if (makapix_store_get_mqtt_host(preserved_mqtt_host, sizeof(preserved_mqtt_host)) == ESP_OK &&
                makapix_store_get_mqtt_port(&preserved_mqtt_port) == ESP_OK) {
                has_preserved_broker = true;
                ESP_LOGI(TAG, "Preserved broker info: %s:%d", preserved_mqtt_host, preserved_mqtt_port);
            }
            
            // Clear old registration data before saving new credentials (only if re-registering)
            // This ensures old data is only cleared upon successful, complete registration
            if (makapix_store_has_player_key() || makapix_store_has_certificates()) {
                ESP_LOGI(TAG, "Clearing old registration data before saving new credentials");
                makapix_store_clear();
            }
            
            // Save certificates to SPIFFS
            err = makapix_store_save_certificates(creds.ca_pem, creds.cert_pem, creds.key_pem);
            if (err == ESP_OK) {
                // Determine which broker info to use:
                // 1. Use credentials response if provided
                // 2. Otherwise use preserved broker info from provisioning
                // 3. Fall back to CONFIG values as last resort
                const char *mqtt_host_to_save;
                uint16_t mqtt_port_to_save;
                
                if (strlen(creds.mqtt_host) > 0 && creds.mqtt_port > 0) {
                    mqtt_host_to_save = creds.mqtt_host;
                    mqtt_port_to_save = creds.mqtt_port;
                    ESP_LOGI(TAG, "Using broker info from credentials response: %s:%d", mqtt_host_to_save, mqtt_port_to_save);
                } else if (has_preserved_broker) {
                    mqtt_host_to_save = preserved_mqtt_host;
                    mqtt_port_to_save = preserved_mqtt_port;
                    ESP_LOGI(TAG, "Using preserved broker info: %s:%d", mqtt_host_to_save, mqtt_port_to_save);
                } else {
                    mqtt_host_to_save = CONFIG_MAKAPIX_CLUB_HOST;
                    mqtt_port_to_save = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
                    ESP_LOGI(TAG, "Using CONFIG broker info: %s:%d", mqtt_host_to_save, mqtt_port_to_save);
                }
                
                // Save broker info (player_key is still valid from this task's scope)
                makapix_store_save_credentials(player_key, mqtt_host_to_save, mqtt_port_to_save);
                
                ESP_LOGI(TAG, "Certificates saved successfully, initiating MQTT connection");
                s_state = MAKAPIX_STATE_CONNECTING;
                
                // Initiate MQTT connection using the determined broker info
                char mqtt_host[64];
                uint16_t mqtt_port;
                snprintf(mqtt_host, sizeof(mqtt_host), "%s", mqtt_host_to_save);
                mqtt_port = mqtt_port_to_save;
                
                // Use certificates directly from creds struct (no need to reload from SPIFFS)
                err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, 
                                       creds.ca_pem, creds.cert_pem, creds.key_pem);
                if (err == ESP_OK) {
                    err = makapix_mqtt_connect();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "MQTT connect failed: %s", esp_err_to_name(err));
                        s_state = MAKAPIX_STATE_DISCONNECTED;
                    }
                } else {
                    ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(err));
                    s_state = MAKAPIX_STATE_DISCONNECTED;
                }
                
                break; // Exit polling task
            } else {
                ESP_LOGE(TAG, "Failed to save certificates: %s", esp_err_to_name(err));
                // Continue polling in case of transient error
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            // Registration not complete yet - continue polling
            ESP_LOGD(TAG, "Credentials not ready yet (404), continuing to poll...");
        } else {
            ESP_LOGW(TAG, "Credential polling error: %s, will retry", esp_err_to_name(err));
            // Continue polling on error
        }
    }

    if (poll_count >= max_polls) {
        ESP_LOGW(TAG, "Credential polling timed out after %d attempts", max_polls);
        s_state = MAKAPIX_STATE_IDLE;
    }

    ESP_LOGI(TAG, "Credential polling task exiting");
    s_poll_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief MQTT reconnection task
 */
static void mqtt_reconnect_task(void *pvParameters)
{
    char player_key[37];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char wifi_ip[16];

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // Wait 5 seconds before retry

        // Check if WiFi has a valid IP before attempting MQTT reconnection
        // This prevents futile reconnection attempts when DHCP lease expired or WiFi is reconnecting
        if (app_wifi_get_local_ip(wifi_ip, sizeof(wifi_ip)) != ESP_OK || 
            strcmp(wifi_ip, "0.0.0.0") == 0) {
            ESP_LOGW(TAG, "WiFi has no valid IP address, skipping MQTT reconnection");
            continue;
        }

        // Get MQTT host/port from store or use CONFIG fallback
        if (makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host)) != ESP_OK) {
            snprintf(mqtt_host, sizeof(mqtt_host), "%s", CONFIG_MAKAPIX_CLUB_HOST);
        }
        if (makapix_store_get_mqtt_port(&mqtt_port) != ESP_OK) {
            mqtt_port = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
        }
        
        if (makapix_store_get_player_key(player_key, sizeof(player_key)) == ESP_OK &&
            makapix_store_has_certificates()) {

            if (!makapix_mqtt_is_connected()) {
                ESP_LOGI(TAG, "=== MQTT RECONNECTION ATTEMPT ===");
                ESP_LOGI(TAG, "WiFi IP: %s", wifi_ip);
                ESP_LOGI(TAG, "Current state: %d", s_state);
                ESP_LOGI(TAG, "Player key: %s", player_key);
                ESP_LOGI(TAG, "MQTT host: %s", mqtt_host);
                ESP_LOGI(TAG, "MQTT port: %d", mqtt_port);
                s_state = MAKAPIX_STATE_CONNECTING;
                
                // Load certificates from SPIFFS
                char ca_cert[4096];
                char client_cert[4096];
                char client_key[4096];
                
                esp_err_t err = makapix_store_get_ca_cert(ca_cert, sizeof(ca_cert));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to load CA cert: %s", esp_err_to_name(err));
                    s_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                err = makapix_store_get_client_cert(client_cert, sizeof(client_cert));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to load client cert: %s", esp_err_to_name(err));
                    s_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                err = makapix_store_get_client_key(client_key, sizeof(client_key));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to load client key: %s", esp_err_to_name(err));
                    s_state = MAKAPIX_STATE_DISCONNECTED;
                    continue;
                }
                
                // Deinit existing client before reinitializing to prevent resource leaks
                makapix_mqtt_deinit();
                
                err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, ca_cert, client_cert, client_key);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "MQTT init successful, attempting connect...");
                    err = makapix_mqtt_connect();
                    if (err != ESP_OK) {
                        s_state = MAKAPIX_STATE_DISCONNECTED;
                        ESP_LOGW(TAG, "MQTT connection failed: %s (%d)", esp_err_to_name(err), err);
                    } else {
                        ESP_LOGI(TAG, "MQTT connect() returned OK, waiting for connection event...");
                    }
                    // State will be updated to CONNECTED by the connection callback when MQTT actually connects
                } else {
                    s_state = MAKAPIX_STATE_DISCONNECTED;
                    ESP_LOGW(TAG, "MQTT init failed: %s (%d)", esp_err_to_name(err), err);
                }
            } else {
                ESP_LOGI(TAG, "MQTT already connected, exiting reconnection task");
                // Already connected, exit task
                break;
            }
        } else {
            // No credentials or certificates, exit task
            if (!makapix_store_has_certificates()) {
                ESP_LOGW(TAG, "Certificates not found, cannot reconnect");
            }
            break;
        }
    }

    ESP_LOGI(TAG, "Reconnection task exiting");
    s_reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t makapix_init(void)
{
    makapix_store_init();

    // Register MQTT connection state callback
    makapix_mqtt_set_connection_callback(mqtt_connection_callback);

    // Initialize MQTT API layer (response correlation). Ignore failure when player_key absent.
    esp_err_t api_err = makapix_api_init();
    if (api_err != ESP_OK) {
        ESP_LOGW(TAG, "makapix_api_init failed (likely no player_key yet): %s", esp_err_to_name(api_err));
    }

    if (makapix_store_has_player_key() && makapix_store_has_certificates()) {
        ESP_LOGI(TAG, "Found stored player_key and certificates, will connect after WiFi");
        s_state = MAKAPIX_STATE_IDLE; // Will transition to CONNECTING when WiFi connects
    } else if (makapix_store_has_player_key()) {
        ESP_LOGI(TAG, "Found stored player_key but no certificates, device needs re-registration");
        s_state = MAKAPIX_STATE_IDLE;
    } else {
        ESP_LOGI(TAG, "No player_key found, waiting for provisioning gesture");
        s_state = MAKAPIX_STATE_IDLE;
    }

    s_current_post_id = 0;
    memset(s_registration_code, 0, sizeof(s_registration_code));
    memset(s_registration_expires, 0, sizeof(s_registration_expires));

    // Create channel switch semaphore and task
    // This task handles all blocking channel switch operations to keep HTTP/MQTT handlers responsive
    if (s_channel_switch_sem == NULL) {
        s_channel_switch_sem = xSemaphoreCreateBinary();
        if (s_channel_switch_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create channel switch semaphore");
            return ESP_ERR_NO_MEM;
        }
    }
    
    if (s_channel_switch_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(channel_switch_task, "ch_switch", 8192, NULL, 5, &s_channel_switch_task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create channel switch task");
            s_channel_switch_task_handle = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Channel switch task created");
    }

    return ESP_OK;
}

makapix_state_t makapix_get_state(void)
{
    return s_state;
}

esp_err_t makapix_start_provisioning(void)
{
    // If already in provisioning/show_code, cancel first
    if (s_state == MAKAPIX_STATE_PROVISIONING || s_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGI(TAG, "Cancelling existing provisioning before starting new one");
        makapix_cancel_provisioning();
        // Wait for polling task to fully exit (up to 15 seconds for HTTP timeout + cleanup)
        if (s_poll_task_handle != NULL) {
            ESP_LOGI(TAG, "Waiting for polling task to exit...");
            int wait_count = 0;
            while (s_poll_task_handle != NULL && wait_count < 150) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_count++;
            }
            if (s_poll_task_handle != NULL) {
                ESP_LOGW(TAG, "Polling task did not exit gracefully");
            }
        }
    }

    ESP_LOGI(TAG, "Starting provisioning...");
    
    // Set initial status message before transitioning state
    snprintf(s_provisioning_status, sizeof(s_provisioning_status), "Starting...");
    
    // Set state to PROVISIONING BEFORE disconnecting MQTT
    // This prevents the disconnect callback from starting a reconnection task
    s_state = MAKAPIX_STATE_PROVISIONING;
    s_provisioning_cancelled = false;  // Reset cancellation flag

    // Stop MQTT client to free network resources for provisioning
    // This prevents MQTT reconnection attempts from interfering with HTTP requests
    if (makapix_mqtt_is_connected()) {
        ESP_LOGI(TAG, "Stopping MQTT client for provisioning...");
        makapix_mqtt_disconnect();
    }

    // Start provisioning task
    BaseType_t ret = xTaskCreate(provisioning_task, "makapix_prov", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create provisioning task");
        s_state = MAKAPIX_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void makapix_cancel_provisioning(void)
{
    if (s_state == MAKAPIX_STATE_PROVISIONING || s_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGI(TAG, "Cancelling provisioning");
        s_provisioning_cancelled = true;  // Set flag to abort provisioning task
        s_state = MAKAPIX_STATE_IDLE;
        memset(s_registration_code, 0, sizeof(s_registration_code));
        memset(s_registration_expires, 0, sizeof(s_registration_expires));
        memset(s_provisioning_status, 0, sizeof(s_provisioning_status));
    }
}

int32_t makapix_get_current_post_id(void)
{
    return s_current_post_id;
}

void makapix_set_current_post_id(int32_t post_id)
{
    s_current_post_id = post_id;
}

bool makapix_get_and_clear_view_intent(void)
{
    bool intentional = s_view_intent_intentional;
    s_view_intent_intentional = false;
    return intentional;
}

esp_err_t makapix_connect_if_registered(void)
{
    if (s_state == MAKAPIX_STATE_CONNECTED || s_state == MAKAPIX_STATE_CONNECTING) {
        ESP_LOGW(TAG, "MQTT already connected or connecting");
        return ESP_OK;
    }

    char player_key[37];
    char mqtt_host[64];
    uint16_t mqtt_port;

    esp_err_t err = makapix_store_get_player_key(player_key, sizeof(player_key));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No player_key stored");
        return ESP_ERR_NOT_FOUND;
    }

    // Check if certificates are available
    if (!makapix_store_has_certificates()) {
        ESP_LOGI(TAG, "Certificates not found, cannot connect to MQTT");
        ESP_LOGI(TAG, "Device needs to complete registration and receive certificates");
        return ESP_ERR_NOT_FOUND;
    }

    // Try to get MQTT host from store, fallback to CONFIG
    err = makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host));
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No MQTT host stored, using CONFIG value: %s", CONFIG_MAKAPIX_CLUB_HOST);
        snprintf(mqtt_host, sizeof(mqtt_host), "%s", CONFIG_MAKAPIX_CLUB_HOST);
    }

    // Try to get MQTT port from store, fallback to CONFIG
    err = makapix_store_get_mqtt_port(&mqtt_port);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No MQTT port stored, using CONFIG value: %d", CONFIG_MAKAPIX_CLUB_MQTT_PORT);
        mqtt_port = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
    }

    ESP_LOGI(TAG, "=== makapix_connect_if_registered START ===");
    ESP_LOGI(TAG, "Current state: %d", s_state);
    ESP_LOGI(TAG, "Stored player_key: %s", player_key);
    ESP_LOGI(TAG, "Stored MQTT host: %s", mqtt_host);
    ESP_LOGI(TAG, "Stored MQTT port: %d", mqtt_port);
    ESP_LOGI(TAG, "Certificates: available");
    ESP_LOGI(TAG, "Connecting to MQTT broker: %s:%d", mqtt_host, mqtt_port);
    s_state = MAKAPIX_STATE_CONNECTING;

    // Load certificates from SPIFFS (allocate dynamically to avoid stack overflow)
    char *ca_cert = malloc(4096);
    char *client_cert = malloc(4096);
    char *client_key = malloc(4096);
    
    if (!ca_cert || !client_cert || !client_key) {
        ESP_LOGE(TAG, "Failed to allocate certificate buffers");
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_state = MAKAPIX_STATE_DISCONNECTED;
        return ESP_ERR_NO_MEM;
    }
    
    err = makapix_store_get_ca_cert(ca_cert, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load CA cert: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }
    
    err = makapix_store_get_client_cert(client_cert, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load client cert: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }
    
    err = makapix_store_get_client_key(client_key, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load client key: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }

    err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, ca_cert, client_cert, client_key);
    
    // Free certificate buffers after passing to mqtt_init (ESP-IDF MQTT client copies them internally)
    free(ca_cert);
    free(client_cert);
    free(client_key);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT: %s (%d)", esp_err_to_name(err), err);
        s_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }

    err = makapix_mqtt_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect MQTT: %s (%d)", esp_err_to_name(err), err);
        ESP_LOGI(TAG, "Starting reconnection task...");
        s_state = MAKAPIX_STATE_DISCONNECTED;
        // Start reconnection task if not already running
        // Stack size needs to be large enough for certificate buffers (3x 4096 bytes = ~12KB)
        if (s_reconnect_task_handle == NULL) {
            BaseType_t task_ret = xTaskCreate(mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle);
            if (task_ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create reconnection task");
                s_reconnect_task_handle = NULL;
            } else {
                ESP_LOGI(TAG, "Reconnection task created successfully");
            }
        } else {
            ESP_LOGI(TAG, "Reconnection task already running");
        }
        return err;
    }

    ESP_LOGI(TAG, "makapix_mqtt_connect() returned OK");
    ESP_LOGI(TAG, "=== makapix_connect_if_registered END ===");

    // State will be updated to CONNECTED by the connection callback when MQTT actually connects
    // Do not set CONNECTED here - connection is asynchronous

    return ESP_OK;
}

esp_err_t makapix_get_registration_code(char *out_code, size_t max_len)
{
    if (!out_code || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(s_registration_code) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(out_code, s_registration_code, max_len - 1);
    out_code[max_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t makapix_get_registration_expires(char *out_expires, size_t max_len)
{
    if (!out_expires || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(s_registration_expires) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(out_expires, s_registration_expires, max_len - 1);
    out_expires[max_len - 1] = '\0';
    return ESP_OK;
}

void makapix_set_provisioning_status(const char *status_message)
{
    if (status_message && s_state == MAKAPIX_STATE_PROVISIONING) {
        strncpy(s_provisioning_status, status_message, sizeof(s_provisioning_status) - 1);
        s_provisioning_status[sizeof(s_provisioning_status) - 1] = '\0';
        ESP_LOGD(TAG, "Provisioning status: %s", s_provisioning_status);
    }
}

/**
 * @brief Get current provisioning status message
 */
esp_err_t makapix_get_provisioning_status(char *out_status, size_t max_len)
{
    if (!out_status || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(s_provisioning_status) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(out_status, s_provisioning_status, max_len - 1);
    out_status[max_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t makapix_switch_to_channel(const char *channel, const char *user_handle)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build channel ID
    char channel_id[128] = {0};
    if (strcmp(channel, "by_user") == 0) {
        if (!user_handle || strlen(user_handle) == 0) {
            ESP_LOGE(TAG, "user_handle required for by_user channel");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(channel_id, sizeof(channel_id), "by_user_%s", user_handle);
    } else {
        strncpy(channel_id, channel, sizeof(channel_id) - 1);
    }
    
    // Check if we're already on this channel - if so, do nothing (no cancellation either)
    if (s_current_channel_id[0] && strcmp(s_current_channel_id, channel_id) == 0 && s_current_channel) {
        ESP_LOGI(TAG, "Already on channel %s - ignoring duplicate switch request", channel_id);
        return ESP_OK;
    }
    
    // Build channel name
    char channel_name[128] = {0};
    if (strcmp(channel, "all") == 0) {
        strcpy(channel_name, "Recent");
    } else if (strcmp(channel, "promoted") == 0) {
        strcpy(channel_name, "Promoted");
    } else if (strcmp(channel, "user") == 0) {
        strcpy(channel_name, "My Artworks");
    } else if (strcmp(channel, "by_user") == 0) {
        snprintf(channel_name, sizeof(channel_name), "%s's Artworks", user_handle);
    } else {
        size_t copy_len = strlen(channel_id);
        if (copy_len >= sizeof(channel_name)) copy_len = sizeof(channel_name) - 1;
        memcpy(channel_name, channel_id, copy_len);
        channel_name[copy_len] = '\0';
    }
    
    // Cancel downloads for the PREVIOUS channel (if different from new channel)
    if (s_current_channel_id[0] && strcmp(s_current_channel_id, channel_id) != 0) {
        ESP_LOGI(TAG, "Cancelling downloads for previous channel: %s", s_current_channel_id);
        download_manager_cancel_channel(s_current_channel_id);
    }
    
    // Mark channel as loading (clear any previous abort state)
    s_channel_loading = true;
    s_channel_load_abort = false;
    strncpy(s_loading_channel_id, channel_id, sizeof(s_loading_channel_id) - 1);
    s_loading_channel_id[sizeof(s_loading_channel_id) - 1] = '\0';
    
    ESP_LOGI(TAG, "Switching to channel: %s (id=%s)", channel_name, channel_id);
    
    // Destroy existing channel if any
    if (s_current_channel) {
        // Clear channel_player pointer BEFORE destroying to prevent race conditions
        channel_player_clear_channel(s_current_channel);
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }
    
    // Create new Makapix channel
    s_current_channel = makapix_channel_create(channel_id, channel_name, "/sdcard/vault", "/sdcard/channel");
    if (!s_current_channel) {
        ESP_LOGE(TAG, "Failed to create channel");
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        return ESP_ERR_NO_MEM;
    }
    
    // Update current channel ID tracker
    strncpy(s_current_channel_id, channel_id, sizeof(s_current_channel_id) - 1);
    s_current_channel_id[sizeof(s_current_channel_id) - 1] = '\0';
    
    // Load channel (will trigger refresh task if index is empty)
    esp_err_t err = channel_load(s_current_channel);
    
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        // Serious error (e.g., refresh task couldn't start due to memory)
        ESP_LOGE(TAG, "Channel load failed: %s", esp_err_to_name(err));
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_ERROR, -1, 
                                       "Failed to load channel");
        channel_player_clear_channel(s_current_channel);
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        s_current_channel_id[0] = '\0';
        p3a_state_fallback_to_sdcard();
        return err;
    }
    
    // Get channel stats - total_items is index entries (not necessarily downloaded)
    channel_stats_t stats = {0};
    channel_get_stats(s_current_channel, &stats);
    
    // Count locally AVAILABLE artworks (files that actually exist)
    // This is different from total_items which includes index entries without files
    size_t available_count = 0;
    size_t post_count = channel_get_post_count(s_current_channel);
    for (size_t i = 0; i < post_count; i++) {
        channel_post_t post = {0};
        if (channel_get_post(s_current_channel, i, &post) == ESP_OK) {
            if (post.kind == CHANNEL_POST_KIND_ARTWORK) {
                struct stat st;
                if (stat(post.u.artwork.filepath, &st) == 0) {
                    available_count++;
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "Channel %s: %zu index entries, %zu locally available", 
             channel_id, stats.total_items, available_count);
    
    // Decision: show loading UI only if ZERO artworks are locally available
    if (available_count == 0) {
        // No local artworks - need to wait for at least one to be downloaded
        ESP_LOGI(TAG, "No local artworks available, waiting for first download...");
        
        // IMMEDIATELY queue initial downloads (don't wait for the 2-second poll)
        makapix_channel_ensure_downloads_ahead(s_current_channel, 16, NULL);
        
        // Set up loading message UI.
        // IMPORTANT: Do NOT switch display render mode here. We keep the display in animation mode
        // and rely on p3a_render to draw the message reliably (avoids blank screen if UI mode fails).
        ugfx_ui_show_channel_message(channel_name, "Loading from Makapix Club...", -1);
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_LOADING, -1,
                                       "Fetching from Makapix Club...");
        
        // Wait for FIRST artwork to become available (not the full batch)
        // This is much faster than waiting for all 32
        const int MAX_WAIT_MS = 60000;
        int waited_ms = 0;
        bool aborted = false;
        bool got_artwork = false;
        
        while (waited_ms < MAX_WAIT_MS && !aborted && !got_artwork) {
            // Check for abort signal first for responsiveness
            if (s_channel_load_abort || s_has_pending_channel) {
                ESP_LOGI(TAG, "Channel load aborted by new request");
                aborted = true;
                break;
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
            waited_ms += 100;
            
            // Re-check available count (files are being downloaded in background)
            size_t new_available = 0;
            size_t new_post_count = channel_get_post_count(s_current_channel);
            for (size_t i = 0; i < new_post_count && !got_artwork; i++) {
                channel_post_t post = {0};
                if (channel_get_post(s_current_channel, i, &post) == ESP_OK) {
                    if (post.kind == CHANNEL_POST_KIND_ARTWORK) {
                        struct stat st;
                        if (stat(post.u.artwork.filepath, &st) == 0) {
                            new_available++;
                            got_artwork = true;  // Found one! Can start playback
                        }
                    }
                }
            }
            
            if (got_artwork) {
                ESP_LOGI(TAG, "First artwork available after %d ms - starting playback!", waited_ms);
                break;
            }
            
            // Update loading message and trigger downloads every 2 seconds
            if (waited_ms % 2000 == 0) {
                ESP_LOGI(TAG, "Still waiting for first artwork... (%d ms)", waited_ms);
                char msg[64];
                snprintf(msg, sizeof(msg), "Loading... (%d sec)", waited_ms / 1000);
                ugfx_ui_show_channel_message(channel_name, msg, -1);
                p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_LOADING, -1, msg);
                
                // Ensure downloads are being queued (in case queue drained or refresh completed)
                makapix_channel_ensure_downloads_ahead(s_current_channel, 16, NULL);
            }
        }
        
        // Clear loading message
        ugfx_ui_hide_channel_message();
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
        
        // Handle abort
        if (aborted) {
            ESP_LOGI(TAG, "Cleaning up aborted channel load for %s", channel_id);
            download_manager_cancel_channel(channel_id);
            channel_player_clear_channel(s_current_channel);
            channel_destroy(s_current_channel);
            s_current_channel = NULL;
            s_channel_loading = false;
            s_loading_channel_id[0] = '\0';
            s_current_channel_id[0] = '\0';
            s_channel_load_abort = false;
            
            char pending_ch[64] = {0};
            char pending_user[64] = {0};
            if (makapix_get_pending_channel(pending_ch, sizeof(pending_ch), pending_user, sizeof(pending_user))) {
                makapix_clear_pending_channel();
                return makapix_switch_to_channel(pending_ch, pending_user[0] ? pending_user : NULL);
            }
            return ESP_ERR_INVALID_STATE;
        }
        
        // Handle timeout (no artwork available after waiting)
        if (!got_artwork) {
            ESP_LOGW(TAG, "Timed out waiting for first artwork");
            p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_EMPTY, -1, 
                                           "No artworks available yet");
            download_manager_cancel_channel(channel_id);
            channel_player_clear_channel(s_current_channel);
            channel_destroy(s_current_channel);
            s_current_channel = NULL;
            s_channel_loading = false;
            s_loading_channel_id[0] = '\0';
            s_current_channel_id[0] = '\0';
            
            char pending_ch[64] = {0};
            char pending_user[64] = {0};
            if (makapix_get_pending_channel(pending_ch, sizeof(pending_ch), pending_user, sizeof(pending_user))) {
                makapix_clear_pending_channel();
                return makapix_switch_to_channel(pending_ch, pending_user[0] ? pending_user : NULL);
            }
            p3a_state_fallback_to_sdcard();
            return ESP_ERR_NOT_FOUND;
        }
    }
    
    // At this point we have at least one locally available artwork - start playback immediately!
    // Background downloads will continue adding more artworks
    
    // Start playback with server order
    err = channel_start_playback(s_current_channel, CHANNEL_ORDER_ORIGINAL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback: %s", esp_err_to_name(err));
        download_manager_cancel_channel(channel_id);
        channel_player_clear_channel(s_current_channel);
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        s_current_channel_id[0] = '\0';
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_ERROR, -1, 
                                       "Failed to start playback");
        p3a_state_fallback_to_sdcard();
        return err;
    }
    
    // Switch the animation player's channel source to this Makapix channel
    err = channel_player_switch_to_makapix_channel(s_current_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch channel player source: %s", esp_err_to_name(err));
        // Continue anyway - channel was created
    }
    
    // Trigger the animation player to load the first artwork
    err = animation_player_request_swap_current();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to trigger initial animation swap: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "Channel switched successfully (background downloads continue)");
    
    // Clear loading state - playback started
    s_channel_loading = false;
    s_loading_channel_id[0] = '\0';

    // Persist "last channel" selection
    if (strcmp(channel, "all") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_ALL, NULL);
    } else if (strcmp(channel, "promoted") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_PROMOTED, NULL);
    } else if (strcmp(channel, "user") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_USER, NULL);
    } else if (strcmp(channel, "by_user") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_BY_USER, user_handle);
    } else {
        ESP_LOGW(TAG, "Not persisting unknown channel key: %s", channel);
    }

    return ESP_OK;
}

esp_err_t makapix_show_artwork(int32_t post_id, const char *storage_key, const char *art_url)
{
    if (!storage_key || !art_url) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Showing artwork: post_id=%ld, storage_key=%s", post_id, storage_key);
    
    // Destroy existing channel if any
    if (s_current_channel) {
        channel_player_clear_channel(s_current_channel);
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }
    
    // Create transient in-memory single-item channel
    channel_handle_t single_ch = create_single_artwork_channel(storage_key, art_url);
    if (!single_ch) {
        ESP_LOGE(TAG, "Failed to create transient artwork channel");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t err = channel_load(single_ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Artwork channel load failed: %s", esp_err_to_name(err));
        channel_destroy(single_ch);
        s_current_channel = NULL;
        // TODO: optionally fallback to sdcard channel here
        return err;
    }
    
    err = channel_start_playback(single_ch, CHANNEL_ORDER_ORIGINAL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Artwork channel start playback failed: %s", esp_err_to_name(err));
        channel_destroy(single_ch);
        s_current_channel = NULL;
        return err;
    }
    
    s_current_channel = single_ch;
    makapix_set_current_post_id(post_id);
    s_view_intent_intentional = true;  // Next buffer swap will submit intentional view
    
    // Switch the animation player's channel source to this transient channel
    err = channel_player_switch_to_makapix_channel(s_current_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch channel player source: %s", esp_err_to_name(err));
    }
    
    // Trigger the animation player to load the artwork
    err = animation_player_request_swap_current();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to trigger animation swap: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "Transient artwork channel created and started");
    return ESP_OK;
}

void makapix_adopt_channel_handle(void *channel)
{
    // NOTE: `channel_handle_t` is opaque and defined in channel_manager. We take void* here to keep makapix.h lightweight.
    // Ownership transfer: if a different channel is already owned, destroy it.
    channel_handle_t ch = (channel_handle_t)channel;
    if (s_current_channel && s_current_channel != ch) {
        channel_player_clear_channel(s_current_channel);
        channel_destroy(s_current_channel);
    }
    s_current_channel = ch;
    
    // Track the channel ID for refresh triggering when MQTT connects
    if (ch) {
        const char *id = makapix_channel_get_id(ch);
        if (id) {
            strncpy(s_current_channel_id, id, sizeof(s_current_channel_id) - 1);
            s_current_channel_id[sizeof(s_current_channel_id) - 1] = '\0';
            ESP_LOGI(TAG, "Adopted channel: %s", s_current_channel_id);
        }
    } else {
        s_current_channel_id[0] = '\0';
    }
}

bool makapix_is_channel_loading(char *out_channel_id, size_t max_len)
{
    if (s_channel_loading && out_channel_id && max_len > 0) {
        strncpy(out_channel_id, s_loading_channel_id, max_len - 1);
        out_channel_id[max_len - 1] = '\0';
    }
    return s_channel_loading;
}

void makapix_abort_channel_load(void)
{
    if (s_channel_loading) {
        ESP_LOGI(TAG, "Signaling abort of channel load: %s", s_loading_channel_id);
        s_channel_load_abort = true;
    }
}

esp_err_t makapix_request_channel_switch(const char *channel, const char *user_handle)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build channel_id for comparison
    char new_channel_id[128] = {0};
    if (strcmp(channel, "by_user") == 0 && user_handle) {
        snprintf(new_channel_id, sizeof(new_channel_id), "by_user_%s", user_handle);
    } else {
        strncpy(new_channel_id, channel, sizeof(new_channel_id) - 1);
    }
    
    // Check if this is the same channel already loading
    if (s_channel_loading && strcmp(s_loading_channel_id, new_channel_id) == 0) {
        ESP_LOGI(TAG, "Channel %s already loading - ignoring duplicate request", channel);
        return ESP_OK;  // Already loading this channel
    }
    
    ESP_LOGI(TAG, "Request channel switch to: %s (loading=%d)", channel, s_channel_loading);
    
    // Store as pending channel
    strncpy(s_pending_channel, channel, sizeof(s_pending_channel) - 1);
    s_pending_channel[sizeof(s_pending_channel) - 1] = '\0';
    
    if (user_handle) {
        strncpy(s_pending_user_handle, user_handle, sizeof(s_pending_user_handle) - 1);
        s_pending_user_handle[sizeof(s_pending_user_handle) - 1] = '\0';
    } else {
        s_pending_user_handle[0] = '\0';
    }
    
    s_has_pending_channel = true;
    
    // If a channel is currently loading, signal abort
    // The channel_switch_task will pick up the pending channel when the current load exits
    if (s_channel_loading) {
        ESP_LOGI(TAG, "Aborting load of %s to switch to %s", s_loading_channel_id, channel);
        s_channel_load_abort = true;
        // Don't signal semaphore - the task will loop back after abort completes
    } else {
        // No channel loading - signal the task to start processing
        if (s_channel_switch_sem) {
            xSemaphoreGive(s_channel_switch_sem);
        }
    }
    
    return ESP_OK;
}

bool makapix_has_pending_channel(void)
{
    return s_has_pending_channel;
}

bool makapix_get_pending_channel(char *out_channel, size_t channel_len, char *out_user_handle, size_t user_len)
{
    if (!s_has_pending_channel) {
        return false;
    }
    
    if (out_channel && channel_len > 0) {
        snprintf(out_channel, channel_len, "%s", s_pending_channel);
    }
    
    if (out_user_handle && user_len > 0) {
        snprintf(out_user_handle, user_len, "%s", s_pending_user_handle);
    }
    
    return true;
}

void makapix_clear_pending_channel(void)
{
    s_has_pending_channel = false;
    s_pending_channel[0] = '\0';
    s_pending_user_handle[0] = '\0';
}

// ---------------------------------------------------------------------------
// Transient in-memory single-artwork channel implementation
// ---------------------------------------------------------------------------

typedef struct {
    struct channel_s base;
    channel_item_ref_t item;
    bool has_item;
    char art_url[256];
} single_artwork_channel_t;

static esp_err_t storage_key_sha256_local(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) return ESP_ERR_INVALID_ARG;
    int ret = mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), out_sha256, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 failed (ret=%d)", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Extension strings for file naming
static const char *s_ext_strings_local[] = { ".webp", ".gif", ".png", ".jpg" };

static int detect_file_type_ext(const char *url)
{
    size_t len = strlen(url);
    // Check longer extensions first (e.g., .jpeg before .jpg), all comparisons are case-insensitive
    if (len >= 5 && strcasecmp(url + len - 5, ".webp") == 0) return 0; // webp
    if (len >= 5 && strcasecmp(url + len - 5, ".jpeg") == 0) return 3; // JPEG (prefer .jpg but accept .jpeg)
    if (len >= 4 && strcasecmp(url + len - 4, ".gif") == 0)  return 1; // gif
    if (len >= 4 && strcasecmp(url + len - 4, ".png") == 0)  return 2; // png
    if (len >= 4 && strcasecmp(url + len - 4, ".jpg") == 0)  return 3; // JPEG (canonical extension)
    return 0;
}

static void build_vault_path_from_storage_key_simple(const char *storage_key, const char *art_url, char *out, size_t out_len)
{
    uint8_t sha256[32];
    if (storage_key_sha256_local(storage_key, sha256) != ESP_OK) {
        snprintf(out, out_len, "%s/%s%s", "/sdcard/vault", storage_key, ".webp");
        return;
    }
    char dir1[3], dir2[3], dir3[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
    snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);
    // Include file extension for type detection
    int ext_idx = detect_file_type_ext(art_url);
    snprintf(out, out_len, "%s/%s/%s/%s/%s%s", "/sdcard/vault", dir1, dir2, dir3, storage_key, s_ext_strings_local[ext_idx]);
}

static esp_err_t single_ch_load(channel_handle_t channel)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    struct stat st;
    if (stat(ch->item.filepath, &st) != 0) {
        // Not present, attempt download with retries
        const int max_attempts = 3;
        for (int attempt = 1; attempt <= max_attempts; attempt++) {
            ESP_LOGI(TAG, "Downloading artwork (attempt %d/%d)...", attempt, max_attempts);
            esp_err_t err = makapix_artwork_download(ch->art_url, ch->item.storage_key, ch->item.filepath, sizeof(ch->item.filepath));
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "Download attempt %d failed: %s", attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (err == ESP_ERR_NOT_FOUND) {
                // Permanent miss (e.g., HTTP 404). Do not retry.
                return ESP_ERR_NOT_FOUND;
            }
            if (attempt == max_attempts) {
                return ESP_FAIL;
            }
        }
    }
    
    ch->has_item = true;
    channel->loaded = true;
    return ESP_OK;
}

static void single_ch_unload(channel_handle_t channel)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch) return;
    ch->has_item = false;
    channel->loaded = false;
}

static esp_err_t single_ch_start_playback(channel_handle_t channel, channel_order_mode_t order_mode, const channel_filter_config_t *filter)
{
    (void)order_mode;
    (void)filter;
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch || !ch->has_item) return ESP_ERR_NOT_FOUND;
    channel->current_order = CHANNEL_ORDER_ORIGINAL;
    channel->current_filter = filter ? *filter : (channel_filter_config_t){0};
    return ESP_OK;
}

static esp_err_t single_ch_next(channel_handle_t channel, channel_item_ref_t *out_item)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch || !ch->has_item) return ESP_ERR_NOT_FOUND;
    if (out_item) *out_item = ch->item;
    return ESP_OK;
}

static esp_err_t single_ch_prev(channel_handle_t channel, channel_item_ref_t *out_item)
{
    return single_ch_next(channel, out_item);
}

static esp_err_t single_ch_current(channel_handle_t channel, channel_item_ref_t *out_item)
{
    return single_ch_next(channel, out_item);
}

static esp_err_t single_ch_request_reshuffle(channel_handle_t channel)
{
    (void)channel;
    return ESP_OK;
}

static esp_err_t single_ch_request_refresh(channel_handle_t channel)
{
    (void)channel;
    return ESP_OK;
}

static esp_err_t single_ch_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!out_stats) return ESP_ERR_INVALID_ARG;
    out_stats->total_items = ch->has_item ? 1 : 0;
    out_stats->filtered_items = out_stats->total_items;
    out_stats->current_position = ch->has_item ? 0 : 0;
    return ESP_OK;
}

static void single_ch_destroy(channel_handle_t channel)
{
    single_artwork_channel_t *ch = (single_artwork_channel_t *)channel;
    if (!ch) return;
    if (ch->base.name) free(ch->base.name);
    free(ch);
}

static const channel_ops_t s_single_ops = {
    .load = single_ch_load,
    .unload = single_ch_unload,
    .start_playback = single_ch_start_playback,
    .next_item = single_ch_next,
    .prev_item = single_ch_prev,
    .current_item = single_ch_current,
    .request_reshuffle = single_ch_request_reshuffle,
    .request_refresh = single_ch_request_refresh,
    .get_stats = single_ch_get_stats,
    .destroy = single_ch_destroy,
};

static channel_handle_t create_single_artwork_channel(const char *storage_key, const char *art_url)
{
    single_artwork_channel_t *ch = calloc(1, sizeof(single_artwork_channel_t));
    if (!ch) return NULL;
    
    ch->base.ops = &s_single_ops;
    ch->base.loaded = false;
    ch->base.current_order = CHANNEL_ORDER_ORIGINAL;
    ch->base.current_filter.required_flags = CHANNEL_FILTER_FLAG_NONE;
    ch->base.current_filter.excluded_flags = CHANNEL_FILTER_FLAG_NONE;
    ch->base.name = strdup("Artwork");
    
    // Build filepath from storage_key (includes extension from art_url)
    build_vault_path_from_storage_key_simple(storage_key, art_url, ch->item.filepath, sizeof(ch->item.filepath));
    strncpy(ch->item.storage_key, storage_key, sizeof(ch->item.storage_key) - 1);
    ch->item.storage_key[sizeof(ch->item.storage_key) - 1] = '\0';
    ch->item.item_index = 0;
    // Set format flags
    switch (detect_file_type_ext(art_url)) {
        case 1: ch->item.flags = CHANNEL_FILTER_FLAG_GIF; break;
        case 2: ch->item.flags = CHANNEL_FILTER_FLAG_PNG; break;
        case 3: ch->item.flags = CHANNEL_FILTER_FLAG_JPEG; break;
        case 0:
        default: ch->item.flags = CHANNEL_FILTER_FLAG_WEBP; break;
    }
    strncpy(ch->art_url, art_url, sizeof(ch->art_url) - 1);
    ch->art_url[sizeof(ch->art_url) - 1] = '\0';
    ch->has_item = false;
    return (channel_handle_t)ch;
}

