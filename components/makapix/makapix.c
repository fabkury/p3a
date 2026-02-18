// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix.c
 * @brief Makapix core module - state management, init, provisioning, connection
 */

#include "makapix_internal.h"
#include "esp_heap_caps.h"
#include "event_bus.h"

// Shared TAG for logging (defined here, declared extern in internal header)
const char *MAKAPIX_TAG = "makapix";

// --------------------------------------------------------------------------
// Shared state (defined here, declared extern in makapix_internal.h)
// --------------------------------------------------------------------------

makapix_state_t s_makapix_state = MAKAPIX_STATE_IDLE;
int32_t s_current_post_id = 0;
bool s_view_intent_intentional = false;  // Track if next view should be intentional

char s_registration_code[8] = {0};  // 6 chars + null + padding to avoid warning
char s_registration_expires[64] = {0};  // Extra space to avoid truncation warning
char s_provisioning_status[128] = {0};  // Status message during provisioning
bool s_provisioning_cancelled = false;  // Flag to prevent race condition

TaskHandle_t s_poll_task_handle = NULL;  // Handle for credential polling task
TaskHandle_t s_reconnect_task_handle = NULL;  // Handle for MQTT reconnection task
TaskHandle_t s_status_publish_task_handle = NULL;  // Handle for status publish task

// PSRAM-backed stack for MQTT reconnection task
static StackType_t *s_mqtt_reconn_stack = NULL;
static StaticTask_t s_mqtt_reconn_task_buffer;

// PSRAM-backed stack for channel switch task
static StackType_t *s_ch_switch_stack = NULL;
static StaticTask_t s_ch_switch_task_buffer;
TaskHandle_t s_channel_switch_task_handle = NULL;  // Handle for channel switch task

TimerHandle_t s_status_timer = NULL;

channel_handle_t s_current_channel = NULL;  // Current active Makapix channel
volatile bool s_channel_loading = false;          // True while a channel is being loaded
volatile bool s_channel_load_abort = false;       // Signal to abort current load
char s_loading_channel_id[128] = {0};             // Channel ID currently being loaded
char s_current_channel_id[128] = {0};             // Channel ID currently active (for download cancellation)
char s_previous_channel_id[128] = {0};            // Previous channel ID (for error fallback)

void makapix_set_state(makapix_state_t new_state)
{
    if (s_makapix_state == new_state) {
        return;
    }

    makapix_state_t old_state = s_makapix_state;
    s_makapix_state = new_state;
    ESP_LOGI(MAKAPIX_TAG, "Makapix state: %d -> %d", old_state, new_state);

    esp_err_t err = event_bus_emit_i32(P3A_EVENT_MAKAPIX_STATE_CHANGED, (int32_t)new_state);
    if (err != ESP_OK) {
        ESP_LOGW(MAKAPIX_TAG, "Failed to emit Makapix state event: %s", esp_err_to_name(err));
    }
}

// Pending channel request (set by handlers, processed by channel switch task)
char s_pending_channel[64] = {0};                 // Requested channel name
char s_pending_identifier[64] = {0};              // User sqid for by_user, hashtag for hashtag
char s_pending_display_handle[64] = {0};          // Display name for UI (e.g., user's display name)
volatile bool s_has_pending_channel = false;      // True if a new channel was requested
SemaphoreHandle_t s_channel_switch_sem = NULL;    // Semaphore to wake channel switch task

// --------------------------------------------------------------------------
// Public API - Initialization
// --------------------------------------------------------------------------

esp_err_t makapix_init(void)
{
    makapix_store_init();

    // Initialize MQTT event signaling for channel refresh coordination
    makapix_channel_events_init();

    // Register MQTT connection state callback
    makapix_mqtt_set_connection_callback(makapix_mqtt_connection_callback);

    // Initialize MQTT API layer (response correlation). Ignore failure when player_key absent.
    esp_err_t api_err = makapix_api_init();
    if (api_err != ESP_OK) {
        ESP_LOGW(MAKAPIX_TAG, "makapix_api_init failed (likely no player_key yet): %s", esp_err_to_name(api_err));
    }

    // Initialize view tracker for timed view events
    esp_err_t view_err = view_tracker_init();
    if (view_err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "view_tracker_init failed: %s", esp_err_to_name(view_err));
    }

    if (makapix_store_has_player_key() && makapix_store_has_certificates()) {
        ESP_LOGD(MAKAPIX_TAG, "Credentials found, will connect after WiFi");
        makapix_set_state(MAKAPIX_STATE_IDLE);
    } else {
        makapix_set_state(MAKAPIX_STATE_IDLE);
    }

    s_current_post_id = 0;
    memset(s_registration_code, 0, sizeof(s_registration_code));
    memset(s_registration_expires, 0, sizeof(s_registration_expires));

    // Create channel switch semaphore and task
    // This task handles all blocking channel switch operations to keep HTTP/MQTT handlers responsive
    if (s_channel_switch_sem == NULL) {
        s_channel_switch_sem = xSemaphoreCreateBinary();
        if (s_channel_switch_sem == NULL) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create channel switch semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_channel_switch_task_handle == NULL) {
        const size_t ch_switch_stack_size = 8192;
        if (!s_ch_switch_stack) {
            s_ch_switch_stack = heap_caps_malloc(ch_switch_stack_size * sizeof(StackType_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

        bool task_created = false;
        if (s_ch_switch_stack) {
            s_channel_switch_task_handle = xTaskCreateStatic(makapix_channel_switch_task, "ch_switch",
                                                              ch_switch_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY,
                                                              s_ch_switch_stack, &s_ch_switch_task_buffer);
            task_created = (s_channel_switch_task_handle != NULL);
        }

        if (!task_created) {
            if (xTaskCreate(makapix_channel_switch_task, "ch_switch",
                            ch_switch_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_channel_switch_task_handle) != pdPASS) {
                ESP_LOGE(MAKAPIX_TAG, "Failed to create channel switch task");
                s_channel_switch_task_handle = NULL;
                return ESP_ERR_NO_MEM;
            }
        }
    }

    return ESP_OK;
}

// --------------------------------------------------------------------------
// Public API - State getters
// --------------------------------------------------------------------------

makapix_state_t makapix_get_state(void)
{
    return s_makapix_state;
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

void makapix_set_view_intent_intentional(bool intentional)
{
    s_view_intent_intentional = intentional;
}

// --------------------------------------------------------------------------
// Public API - Provisioning
// --------------------------------------------------------------------------

esp_err_t makapix_start_provisioning(void)
{
    // If registration was marked invalid, clear it so provisioning can proceed
    if (s_makapix_state == MAKAPIX_STATE_REGISTRATION_INVALID) {
        ESP_LOGI(MAKAPIX_TAG, "Clearing invalid registration state for fresh provisioning");
        makapix_set_state(MAKAPIX_STATE_IDLE);
        makapix_mqtt_reset_auth_failure_count();
    }

    // If already in provisioning/show_code, cancel first
    if (s_makapix_state == MAKAPIX_STATE_PROVISIONING || s_makapix_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGD(MAKAPIX_TAG, "Cancelling existing provisioning before starting new one");
        makapix_cancel_provisioning();
        // Wait for polling task to fully exit (up to 15 seconds for HTTP timeout + cleanup)
        if (s_poll_task_handle != NULL) {
            ESP_LOGD(MAKAPIX_TAG, "Waiting for polling task to exit...");
            int wait_count = 0;
            while (s_poll_task_handle != NULL && wait_count < 150) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_count++;
            }
            if (s_poll_task_handle != NULL) {
                ESP_LOGW(MAKAPIX_TAG, "Polling task did not exit gracefully");
            }
        }
    }

    ESP_LOGD(MAKAPIX_TAG, "Starting provisioning...");

    // Set initial status message before transitioning state
    snprintf(s_provisioning_status, sizeof(s_provisioning_status), "Starting...");

    // Set state to PROVISIONING BEFORE disconnecting MQTT
    // This prevents the disconnect callback from starting a reconnection task
    makapix_set_state(MAKAPIX_STATE_PROVISIONING);
    s_provisioning_cancelled = false;  // Reset cancellation flag

    // Stop MQTT client to free network resources for provisioning
    // This prevents MQTT reconnection attempts from interfering with HTTP requests
    if (makapix_mqtt_is_connected()) {
        ESP_LOGD(MAKAPIX_TAG, "Stopping MQTT client for provisioning...");
        makapix_mqtt_disconnect();
    }

    // Start provisioning task
    BaseType_t ret = xTaskCreate(makapix_provisioning_task, "makapix_prov", 8192, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create provisioning task");
        makapix_set_state(MAKAPIX_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void makapix_cancel_provisioning(void)
{
    if (s_makapix_state == MAKAPIX_STATE_PROVISIONING || s_makapix_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGD(MAKAPIX_TAG, "Cancelling provisioning");
        s_provisioning_cancelled = true;  // Set flag to abort provisioning task
        makapix_set_state(MAKAPIX_STATE_IDLE);
        memset(s_registration_code, 0, sizeof(s_registration_code));
        memset(s_registration_expires, 0, sizeof(s_registration_expires));
        memset(s_provisioning_status, 0, sizeof(s_provisioning_status));
    }
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
    if (status_message && s_makapix_state == MAKAPIX_STATE_PROVISIONING) {
        strncpy(s_provisioning_status, status_message, sizeof(s_provisioning_status) - 1);
        s_provisioning_status[sizeof(s_provisioning_status) - 1] = '\0';
        ESP_LOGD(MAKAPIX_TAG, "Provisioning status: %s", s_provisioning_status);
        event_bus_emit_ptr(P3A_EVENT_PROVISIONING_STATUS_CHANGED, s_provisioning_status);
    }
}

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

// --------------------------------------------------------------------------
// Public API - Connection
// --------------------------------------------------------------------------

esp_err_t makapix_connect_if_registered(void)
{
    if (s_makapix_state == MAKAPIX_STATE_REGISTRATION_INVALID) {
        ESP_LOGW(MAKAPIX_TAG, "Registration is invalid, not attempting MQTT connection");
        ESP_LOGW(MAKAPIX_TAG, "Re-provision device to restore Makapix Club connectivity");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_makapix_state == MAKAPIX_STATE_CONNECTED || s_makapix_state == MAKAPIX_STATE_CONNECTING) {
        ESP_LOGW(MAKAPIX_TAG, "MQTT already connected or connecting");
        return ESP_OK;
    }

    char player_key[37];
    char mqtt_host[64];
    uint16_t mqtt_port;

    esp_err_t err = makapix_store_get_player_key(player_key, sizeof(player_key));
    if (err != ESP_OK) {
        ESP_LOGD(MAKAPIX_TAG, "No player_key stored");
        return ESP_ERR_NOT_FOUND;
    }

    // Check if certificates are available
    if (!makapix_store_has_certificates()) {
        ESP_LOGD(MAKAPIX_TAG, "Certificates not found, cannot connect to MQTT");
        ESP_LOGD(MAKAPIX_TAG, "Device needs to complete registration and receive certificates");
        return ESP_ERR_NOT_FOUND;
    }

    // Try to get MQTT host from store, fallback to CONFIG
    err = makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host));
    if (err != ESP_OK) {
        ESP_LOGD(MAKAPIX_TAG, "No MQTT host stored, using CONFIG value: %s", CONFIG_MAKAPIX_CLUB_HOST);
        snprintf(mqtt_host, sizeof(mqtt_host), "%s", CONFIG_MAKAPIX_CLUB_HOST);
    }

    // Try to get MQTT port from store, fallback to CONFIG
    err = makapix_store_get_mqtt_port(&mqtt_port);
    if (err != ESP_OK) {
        ESP_LOGD(MAKAPIX_TAG, "No MQTT port stored, using CONFIG value: %d", CONFIG_MAKAPIX_CLUB_MQTT_PORT);
        mqtt_port = CONFIG_MAKAPIX_CLUB_MQTT_PORT;
    }

    ESP_LOGD(MAKAPIX_TAG, "Connecting to %s:%d", mqtt_host, mqtt_port);
    makapix_set_state(MAKAPIX_STATE_CONNECTING);

    // Load certificates from SPIFFS (allocate dynamically to avoid stack overflow)
    char *ca_cert = malloc(4096);
    char *client_cert = malloc(4096);
    char *client_key = malloc(4096);

    if (!ca_cert || !client_cert || !client_key) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to allocate certificate buffers");
        free(ca_cert);
        free(client_cert);
        free(client_key);
        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
        return ESP_ERR_NO_MEM;
    }

    err = makapix_store_get_ca_cert(ca_cert, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to load CA cert: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
        return err;
    }

    err = makapix_store_get_client_cert(client_cert, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to load client cert: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
        return err;
    }

    err = makapix_store_get_client_key(client_key, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to load client key: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
        return err;
    }

    err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, ca_cert, client_cert, client_key);

    // Free certificate buffers after passing to mqtt_init (ESP-IDF MQTT client copies them internally)
    free(ca_cert);
    free(client_cert);
    free(client_key);

    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to initialize MQTT: %s (%d)", esp_err_to_name(err), err);
        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
        return err;
    }

    err = makapix_mqtt_connect();
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to connect: %s", esp_err_to_name(err));
        makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
        if (s_reconnect_task_handle == NULL) {
            const size_t mqtt_reconn_stack_size = 16384;
            if (!s_mqtt_reconn_stack) {
                s_mqtt_reconn_stack = heap_caps_malloc(mqtt_reconn_stack_size * sizeof(StackType_t),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }

            bool task_created = false;
            if (s_mqtt_reconn_stack) {
                s_reconnect_task_handle = xTaskCreateStatic(makapix_mqtt_reconnect_task, "mqtt_reconn",
                                                             mqtt_reconn_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY,
                                                             s_mqtt_reconn_stack, &s_mqtt_reconn_task_buffer);
                task_created = (s_reconnect_task_handle != NULL);
            }

            if (!task_created) {
                if (xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn",
                                mqtt_reconn_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_reconnect_task_handle) != pdPASS) {
                    s_reconnect_task_handle = NULL;
                }
            }
        }
        return err;
    }

    return ESP_OK;
}
