#include "makapix.h"
#include "makapix_store.h"
#include "makapix_provision.h"
#include "makapix_mqtt.h"
#include "makapix_api.h"
#include "makapix_channel_impl.h"
#include "makapix_artwork.h"
#include "app_wifi.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
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
static channel_handle_t s_current_channel = NULL;  // Current active Makapix channel

// Forward declarations
static void credentials_poll_task(void *pvParameters);
static void mqtt_reconnect_task(void *pvParameters);
static channel_handle_t create_single_artwork_channel(const char *storage_key, const char *art_url);

#define STATUS_PUBLISH_INTERVAL_MS (30000) // 30 seconds

/**
 * @brief Timer callback for periodic status publishing
 */
static void status_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (makapix_mqtt_is_connected()) {
        makapix_mqtt_publish_status(makapix_get_current_post_id());
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
    } else {
        ESP_LOGI(TAG, "MQTT disconnected");
        
        // Stop status timer to prevent publish attempts during reconnection
        if (s_status_timer) {
            xTimerStop(s_status_timer, 0);
            ESP_LOGI(TAG, "Status timer stopped");
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
    
    ESP_LOGI(TAG, "Switching to channel: %s (id=%s)", channel_name, channel_id);
    
    // Destroy existing channel if any
    if (s_current_channel) {
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }
    
    // Create new Makapix channel
    s_current_channel = makapix_channel_create(channel_id, channel_name, "/sdcard/vault", "/sdcard/channels");
    if (!s_current_channel) {
        ESP_LOGE(TAG, "Failed to create channel");
        return ESP_ERR_NO_MEM;
    }
    
    // Load channel (will trigger refresh if empty)
    esp_err_t err = channel_load(s_current_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Channel load returned %s (may be empty, refresh will populate)", esp_err_to_name(err));
    }
    
    // Check if channel is empty (first-time access)
    channel_stats_t stats = {0};
    channel_get_stats(s_current_channel, &stats);
    
    if (stats.total_items == 0) {
        // Channel is empty - show loading message and wait for data
        ESP_LOGI(TAG, "Channel empty, waiting for data from Makapix Club...");
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_LOADING, -1, 
                                       "Fetching from Makapix Club...");
        
        // Wait for refresh task to populate the channel (max 30 seconds)
        const int MAX_WAIT_MS = 30000;
        const int POLL_INTERVAL_MS = 500;
        int waited_ms = 0;
        
        while (waited_ms < MAX_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            waited_ms += POLL_INTERVAL_MS;
            
            // Check if channel now has entries
            channel_get_stats(s_current_channel, &stats);
            if (stats.total_items > 0) {
                ESP_LOGI(TAG, "Channel now has %zu entries after %d ms", stats.total_items, waited_ms);
                break;
            }
            
            // Update loading message with elapsed time
            if (waited_ms % 2000 == 0) {
                ESP_LOGI(TAG, "Still waiting for channel data... (%d ms)", waited_ms);
            }
        }
        
        // Clear channel message
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
        
        if (stats.total_items == 0) {
            ESP_LOGW(TAG, "Timed out waiting for channel data");
            p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_EMPTY, -1, 
                                           "No artworks available yet");
        }
    }
    
    // Start playback with server order
    err = channel_start_playback(s_current_channel, CHANNEL_ORDER_ORIGINAL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start playback: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "Channel switched successfully");
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
    
    ESP_LOGI(TAG, "Transient artwork channel created and started");
    return ESP_OK;
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

static uint32_t hash_string_local(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static int detect_file_type_ext(const char *url)
{
    size_t len = strlen(url);
    if (len >= 5 && strcasecmp(url + len - 5, ".webp") == 0) return 0; // webp
    if (len >= 4 && strcasecmp(url + len - 4, ".gif") == 0)  return 1; // gif
    if (len >= 4 && strcasecmp(url + len - 4, ".png") == 0)  return 2; // png
    if (len >= 4 && strcasecmp(url + len - 4, ".jpg") == 0)  return 3; // jpg
    if (len >= 5 && strcasecmp(url + len - 5, ".jpeg") == 0) return 3; // jpg
    return 0;
}

static void build_vault_path_from_storage_key_simple(const char *storage_key, char *out, size_t out_len)
{
    uint32_t hash = hash_string_local(storage_key);
    char dir1[3], dir2[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)((hash >> 24) & 0xFF));
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)((hash >> 16) & 0xFF));
    snprintf(out, out_len, "%s/%s/%s/%s", "/sdcard/vault", dir1, dir2, storage_key);
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
    
    // Build filepath from storage_key
    build_vault_path_from_storage_key_simple(storage_key, ch->item.filepath, sizeof(ch->item.filepath));
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

