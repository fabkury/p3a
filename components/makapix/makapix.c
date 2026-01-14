// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file makapix.c
 * @brief Makapix core module - state management, init, channel operations
 */

#include "makapix_internal.h"
#include "mbedtls/sha256.h"
#include "config_store.h"
#include "connectivity_state.h"

// Helper to map global play_order setting to channel_order_mode
static channel_order_mode_t get_global_channel_order(void)
{
    uint8_t play_order = config_store_get_play_order();
    switch (play_order) {
        case 1: return CHANNEL_ORDER_CREATED;
        case 2: return CHANNEL_ORDER_RANDOM;
        case 0:
        default:
            return CHANNEL_ORDER_ORIGINAL;
    }
}

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
TaskHandle_t s_channel_switch_task_handle = NULL;  // Handle for channel switch task

TimerHandle_t s_status_timer = NULL;

channel_handle_t s_current_channel = NULL;  // Current active Makapix channel
volatile bool s_channel_loading = false;          // True while a channel is being loaded
volatile bool s_channel_load_abort = false;       // Signal to abort current load
char s_loading_channel_id[128] = {0};             // Channel ID currently being loaded
char s_current_channel_id[128] = {0};             // Channel ID currently active (for download cancellation)
char s_previous_channel_id[128] = {0};            // Previous channel ID (for error fallback)

// Pending channel request (set by handlers, processed by channel switch task)
char s_pending_channel[64] = {0};                 // Requested channel name
char s_pending_user_handle[64] = {0};             // User handle for by_user channel
volatile bool s_has_pending_channel = false;      // True if a new channel was requested
SemaphoreHandle_t s_channel_switch_sem = NULL;    // Semaphore to wake channel switch task

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------

static channel_handle_t create_single_artwork_channel(const char *storage_key, const char *art_url);

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
        s_makapix_state = MAKAPIX_STATE_IDLE;
    } else {
        s_makapix_state = MAKAPIX_STATE_IDLE;
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
        BaseType_t task_ret = xTaskCreate(makapix_channel_switch_task, "ch_switch", 8192, NULL, 5, &s_channel_switch_task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create channel switch task");
            s_channel_switch_task_handle = NULL;
            return ESP_ERR_NO_MEM;
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

// --------------------------------------------------------------------------
// Public API - Provisioning
// --------------------------------------------------------------------------

esp_err_t makapix_start_provisioning(void)
{
    // If registration was marked invalid, clear it so provisioning can proceed
    if (s_makapix_state == MAKAPIX_STATE_REGISTRATION_INVALID) {
        ESP_LOGI(MAKAPIX_TAG, "Clearing invalid registration state for fresh provisioning");
        s_makapix_state = MAKAPIX_STATE_IDLE;
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
    s_makapix_state = MAKAPIX_STATE_PROVISIONING;
    s_provisioning_cancelled = false;  // Reset cancellation flag

    // Stop MQTT client to free network resources for provisioning
    // This prevents MQTT reconnection attempts from interfering with HTTP requests
    if (makapix_mqtt_is_connected()) {
        ESP_LOGD(MAKAPIX_TAG, "Stopping MQTT client for provisioning...");
        makapix_mqtt_disconnect();
    }

    // Start provisioning task
    BaseType_t ret = xTaskCreate(makapix_provisioning_task, "makapix_prov", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create provisioning task");
        s_makapix_state = MAKAPIX_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void makapix_cancel_provisioning(void)
{
    if (s_makapix_state == MAKAPIX_STATE_PROVISIONING || s_makapix_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGD(MAKAPIX_TAG, "Cancelling provisioning");
        s_provisioning_cancelled = true;  // Set flag to abort provisioning task
        s_makapix_state = MAKAPIX_STATE_IDLE;
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
    s_makapix_state = MAKAPIX_STATE_CONNECTING;

    // Load certificates from SPIFFS (allocate dynamically to avoid stack overflow)
    char *ca_cert = malloc(4096);
    char *client_cert = malloc(4096);
    char *client_key = malloc(4096);
    
    if (!ca_cert || !client_cert || !client_key) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to allocate certificate buffers");
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
        return ESP_ERR_NO_MEM;
    }
    
    err = makapix_store_get_ca_cert(ca_cert, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to load CA cert: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }
    
    err = makapix_store_get_client_cert(client_cert, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to load client cert: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }
    
    err = makapix_store_get_client_key(client_key, 4096);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to load client key: %s", esp_err_to_name(err));
        free(ca_cert);
        free(client_cert);
        free(client_key);
        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }

    err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port, ca_cert, client_cert, client_key);
    
    // Free certificate buffers after passing to mqtt_init (ESP-IDF MQTT client copies them internally)
    free(ca_cert);
    free(client_cert);
    free(client_key);
    
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to initialize MQTT: %s (%d)", esp_err_to_name(err), err);
        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }

    err = makapix_mqtt_connect();
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to connect: %s", esp_err_to_name(err));
        s_makapix_state = MAKAPIX_STATE_DISCONNECTED;
        if (s_reconnect_task_handle == NULL) {
            if (xTaskCreate(makapix_mqtt_reconnect_task, "mqtt_reconn", 16384, NULL, 5, &s_reconnect_task_handle) != pdPASS) {
                s_reconnect_task_handle = NULL;
            }
        }
        return err;
    }

    return ESP_OK;
}

// --------------------------------------------------------------------------
// Public API - Channel switching
// --------------------------------------------------------------------------

esp_err_t makapix_switch_to_channel(const char *channel, const char *identifier)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build channel ID
    char channel_id[128] = {0};
    if (strcmp(channel, "by_user") == 0) {
        if (!identifier || strlen(identifier) == 0) {
            ESP_LOGE(MAKAPIX_TAG, "identifier required for by_user channel");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(channel_id, sizeof(channel_id), "by_user_%s", identifier);
    } else if (strcmp(channel, "hashtag") == 0) {
        if (!identifier || strlen(identifier) == 0) {
            ESP_LOGE(MAKAPIX_TAG, "identifier required for hashtag channel");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(channel_id, sizeof(channel_id), "hashtag_%s", identifier);
    } else {
        strncpy(channel_id, channel, sizeof(channel_id) - 1);
    }
    
    // Check if we're already on this channel
    if (s_current_channel_id[0] && strcmp(s_current_channel_id, channel_id) == 0 && s_current_channel) {
        ESP_LOGI(MAKAPIX_TAG, "Already on channel %s, restarting playback without refresh", channel_id);
        // Restart playback but don't re-trigger refresh
        esp_err_t err = channel_start_playback(s_current_channel, get_global_channel_order(), NULL);
        if (err != ESP_OK) {
            ESP_LOGW(MAKAPIX_TAG, "Failed to restart playback: %s", esp_err_to_name(err));
        }
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
        snprintf(channel_name, sizeof(channel_name), "@%s's Artworks", identifier);
    } else if (strcmp(channel, "hashtag") == 0) {
        snprintf(channel_name, sizeof(channel_name), "#%s", identifier);
    } else {
        size_t copy_len = strlen(channel_id);
        if (copy_len >= sizeof(channel_name)) copy_len = sizeof(channel_name) - 1;
        memcpy(channel_name, channel_id, copy_len);
        channel_name[copy_len] = '\0';
    }
    
    // Store previous channel ID for error fallback
    strncpy(s_previous_channel_id, s_current_channel_id, sizeof(s_previous_channel_id) - 1);
    s_previous_channel_id[sizeof(s_previous_channel_id) - 1] = '\0';
    
    // Mark channel as loading (clear any previous abort state)
    s_channel_loading = true;
    s_channel_load_abort = false;
    strncpy(s_loading_channel_id, channel_id, sizeof(s_loading_channel_id) - 1);
    s_loading_channel_id[sizeof(s_loading_channel_id) - 1] = '\0';
    
    ESP_LOGD(MAKAPIX_TAG, "Switching to channel: %s", channel_name);
    
    // Destroy existing channel if any
    if (s_current_channel) {
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }
    
    // Get dynamic paths
    char vault_path[128], channel_path[128];
    sd_path_get_vault(vault_path, sizeof(vault_path));
    sd_path_get_channel(channel_path, sizeof(channel_path));
    
    // Create new Makapix channel
    s_current_channel = makapix_channel_create(channel_id, channel_name, vault_path, channel_path);
    if (!s_current_channel) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create channel");
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
        ESP_LOGE(MAKAPIX_TAG, "Channel load failed: %s", esp_err_to_name(err));
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_ERROR, -1,
                                       "Failed to load channel");
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
        s_channel_loading = false;
        s_loading_channel_id[0] = '\0';
        s_current_channel_id[0] = '\0';
        p3a_state_fallback_to_sdcard();
        return err;
    }
    
    // Show "Connecting..." message if MQTT not yet connected
    // The refresh task is waiting for MQTT, so let the user know what's happening
    // But only if we have WiFi connectivity (no point in AP mode)
    if (!makapix_mqtt_is_connected() && connectivity_state_has_wifi()) {
        ESP_LOGD(MAKAPIX_TAG, "MQTT not connected, showing 'Connecting...' message");
        p3a_render_set_channel_message(channel_name, P3A_CHANNEL_MSG_LOADING, -1, 
                                       "Connecting to Makapix Club...");
        p3a_channel_message_t msg = {
            .type = P3A_CHANNEL_MSG_LOADING,
            .progress_percent = -1
        };
        snprintf(msg.channel_name, sizeof(msg.channel_name), "%s", channel_name);
        snprintf(msg.detail, sizeof(msg.detail), "Connecting to Makapix Club...");
        p3a_state_set_channel_message(&msg);
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
    
    ESP_LOGD(MAKAPIX_TAG, "Channel %s: %zu entries, %zu available",
             channel_id, stats.total_items, available_count);

    // Display title for UI messages is always "Makapix Club"
    const char *ui_title = "Makapix Club";
    
    // Decision: show loading UI only if ZERO artworks are locally available
    if (available_count == 0) {
        
        // Set up loading message UI based on channel state
        // IMPORTANT: Do NOT switch display render mode here. We keep the display in animation mode
        // and rely on p3a_render to draw the message reliably (avoids blank screen if UI mode fails).
        // Only show loading messages if we have WiFi connectivity (no point in AP mode)
        if (connectivity_state_has_wifi()) {
            const char *loading_message;
            if (stats.total_items == 0) {
                // Empty index - waiting for refresh to populate
                loading_message = "Updating channel index...";
                ugfx_ui_show_channel_message(ui_title, loading_message, -1);
                p3a_render_set_channel_message(ui_title, P3A_CHANNEL_MSG_LOADING, -1, loading_message);
            } else {
                // Has index but no downloaded files yet - show "Waiting for download..."
                // The actual "Downloading artwork..." will be shown when download starts
                loading_message = "Waiting for download...";
                ugfx_ui_show_channel_message(ui_title, loading_message, -1);
                p3a_render_set_channel_message(ui_title, P3A_CHANNEL_MSG_DOWNLOADING, -1, loading_message);
            }
        }
        
        // Wait for FIRST artwork to become available using polling approach
        // We poll every 500ms to check for available files and update UI every 2 seconds
        const int MAX_WAIT_MS = 60000;
        const int POLL_INTERVAL_MS = 500;
        const int UI_UPDATE_INTERVAL_MS = 2000;
        int64_t start_time = esp_timer_get_time() / 1000;  // Convert to ms
        int64_t last_ui_update = 0;
        bool aborted = false;
        bool got_artwork = false;
        
        while (!aborted && !got_artwork) {
            int64_t now = esp_timer_get_time() / 1000;
            int64_t elapsed_ms = now - start_time;
            
            // Check timeout
            if (elapsed_ms >= MAX_WAIT_MS) {
                ESP_LOGW(MAKAPIX_TAG, "Timed out waiting for first artwork after %lld ms", (long long)elapsed_ms);
                break;
            }
            
            // Check if playback has already started (play_scheduler may have triggered it)
            extern bool animation_player_is_animation_ready(void) __attribute__((weak));
            if (animation_player_is_animation_ready && animation_player_is_animation_ready()) {
                ESP_LOGI(MAKAPIX_TAG, "Playback already started, exiting wait loop");
                got_artwork = true;  // Don't show any more messages
                break;
            }
            
            // Check for abort signal first for responsiveness
            if (s_channel_load_abort || s_has_pending_channel) {
                ESP_LOGD(MAKAPIX_TAG, "Channel load aborted by new request");
                aborted = true;
                break;
            }
            
            // Check for available artwork
            size_t new_post_count = channel_get_post_count(s_current_channel);
            for (size_t i = 0; i < new_post_count && !got_artwork; i++) {
                channel_post_t post = {0};
                if (channel_get_post(s_current_channel, i, &post) == ESP_OK) {
                    if (post.kind == CHANNEL_POST_KIND_ARTWORK) {
                        struct stat st;
                        if (stat(post.u.artwork.filepath, &st) == 0) {
                            got_artwork = true;  // Found one! Can start playback
                            ESP_LOGI(MAKAPIX_TAG, "First artwork available after %lld ms", (long long)elapsed_ms);
                        }
                    }
                }
            }
            
            if (got_artwork) {
                break;
            }
            
            // Update loading message every UI_UPDATE_INTERVAL_MS
            if (elapsed_ms - last_ui_update >= UI_UPDATE_INTERVAL_MS) {
                last_ui_update = elapsed_ms;
                size_t current_total = channel_get_post_count(s_current_channel);
                char msg[64];
                int msg_type;
                if (current_total == 0) {
                    snprintf(msg, sizeof(msg), "Updating index... (%lld sec)", (long long)(elapsed_ms / 1000));
                    msg_type = P3A_CHANNEL_MSG_LOADING;
                } else {
                    // Check if download is actively happening
                    if (download_manager_is_busy()) {
                        snprintf(msg, sizeof(msg), "Downloading artwork...");
                    } else {
                        snprintf(msg, sizeof(msg), "Waiting for download...");
                    }
                    msg_type = P3A_CHANNEL_MSG_DOWNLOADING;
                }
                ugfx_ui_show_channel_message(ui_title, msg, -1);
                p3a_render_set_channel_message(ui_title, msg_type, -1, msg);
            }
            
            // Wait a bit before checking again
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        }
        
        // Clear loading message
        ugfx_ui_hide_channel_message();
        p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
        
        // Handle abort
        if (aborted) {
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
        
        // Handle timeout
        if (!got_artwork) {
            ESP_LOGW(MAKAPIX_TAG, "Timed out waiting for artwork");

            // Clean up current channel
            channel_destroy(s_current_channel);
            s_current_channel = NULL;
            s_channel_loading = false;
            s_loading_channel_id[0] = '\0';
            s_current_channel_id[0] = '\0';
            
            // Check for pending channel first
            char pending_ch[64] = {0};
            char pending_user[64] = {0};
            if (makapix_get_pending_channel(pending_ch, sizeof(pending_ch), pending_user, sizeof(pending_user))) {
                makapix_clear_pending_channel();
                // Clear loading message before switching
                ugfx_ui_hide_channel_message();
                p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                return makapix_switch_to_channel(pending_ch, pending_user[0] ? pending_user : NULL);
            }
            
            // Fall back to previous channel if available
            if (s_previous_channel_id[0] != '\0') {
                // Clear loading message before switching
                ugfx_ui_hide_channel_message();
                p3a_render_set_channel_message(NULL, P3A_CHANNEL_MSG_NONE, -1, NULL);
                // Parse previous channel to extract channel type and identifier
                char prev_channel[64] = {0};
                char prev_identifier[64] = {0};
                if (strncmp(s_previous_channel_id, "by_user_", 8) == 0) {
                    snprintf(prev_channel, sizeof(prev_channel), "by_user");
                    snprintf(prev_identifier, sizeof(prev_identifier), "%.63s", s_previous_channel_id + 8);
                } else if (strncmp(s_previous_channel_id, "hashtag_", 8) == 0) {
                    snprintf(prev_channel, sizeof(prev_channel), "hashtag");
                    snprintf(prev_identifier, sizeof(prev_identifier), "%.63s", s_previous_channel_id + 8);
                } else {
                    snprintf(prev_channel, sizeof(prev_channel), "%.63s", s_previous_channel_id);
                }
                return makapix_switch_to_channel(prev_channel, prev_identifier[0] ? prev_identifier : NULL);
            }
            
            // No previous channel - fall back to SD card
            // Don't clear channel message - let fallback function show appropriate message
            // (it will show "No artworks available" if SD card is also empty)
            p3a_state_fallback_to_sdcard();
            return ESP_ERR_NOT_FOUND;
        }
    }
    
    // At this point we have at least one locally available artwork - start playback immediately!
    // Background downloads will continue adding more artworks
    
    // Start playback with global play order setting
    err = channel_start_playback(s_current_channel, get_global_channel_order(), NULL);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to start playback: %s", esp_err_to_name(err));
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

    // Switch play_scheduler to this channel and start playback
    if (strcmp(channel, "by_user") == 0 && identifier) {
        err = play_scheduler_play_user_channel(identifier);
    } else if (strcmp(channel, "hashtag") == 0 && identifier) {
        err = play_scheduler_play_hashtag_channel(identifier);
    } else {
        // "all", "promoted", or other named channels
        err = play_scheduler_play_named_channel(channel);
    }
    if (err != ESP_OK) {
        ESP_LOGW(MAKAPIX_TAG, "Failed to initiate play_scheduler: %s", esp_err_to_name(err));
    }
    
    ESP_LOGD(MAKAPIX_TAG, "Channel %s ready, playback initiated", channel_name);
    
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
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_BY_USER, identifier);
    } else if (strcmp(channel, "hashtag") == 0) {
        (void)p3a_state_switch_channel(P3A_CHANNEL_MAKAPIX_HASHTAG, identifier);
    } else {
        ESP_LOGW(MAKAPIX_TAG, "Not persisting unknown channel key: %s", channel);
    }

    return ESP_OK;
}

esp_err_t makapix_show_artwork(int32_t post_id, const char *storage_key, const char *art_url)
{
    if (!storage_key || !art_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(MAKAPIX_TAG, "Showing artwork: post_id=%ld, storage_key=%s", post_id, storage_key);

    // Destroy existing channel if any
    if (s_current_channel) {
        channel_destroy(s_current_channel);
        s_current_channel = NULL;
    }

    // Create transient in-memory single-item channel
    channel_handle_t single_ch = create_single_artwork_channel(storage_key, art_url);
    if (!single_ch) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create transient artwork channel");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = channel_load(single_ch);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Artwork channel load failed: %s", esp_err_to_name(err));
        channel_destroy(single_ch);
        s_current_channel = NULL;
        // TODO: optionally fallback to sdcard channel here
        return err;
    }

    err = channel_start_playback(single_ch, get_global_channel_order(), NULL);
    if (err != ESP_OK) {
        ESP_LOGE(MAKAPIX_TAG, "Artwork channel start playback failed: %s", esp_err_to_name(err));
        channel_destroy(single_ch);
        s_current_channel = NULL;
        return err;
    }

    s_current_channel = single_ch;
    makapix_set_current_post_id(post_id);
    s_view_intent_intentional = true;  // Next buffer swap will submit intentional view

    // Start playback using play_scheduler
    // Note: This is a transient single-artwork channel, use next() to trigger swap
    err = play_scheduler_next(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(MAKAPIX_TAG, "Failed to initiate artwork swap: %s", esp_err_to_name(err));
    }

    ESP_LOGD(MAKAPIX_TAG, "Transient artwork channel created and started");
    return ESP_OK;
}

void makapix_adopt_channel_handle(void *channel)
{
    // NOTE: `channel_handle_t` is opaque and defined in channel_manager. We take void* here to keep makapix.h lightweight.
    // Ownership transfer: if a different channel is already owned, destroy it.
    channel_handle_t ch = (channel_handle_t)channel;
    if (s_current_channel && s_current_channel != ch) {
        channel_destroy(s_current_channel);
    }
    s_current_channel = ch;
    
    // Track the channel ID for refresh triggering when MQTT connects
    if (ch) {
        const char *id = makapix_channel_get_id(ch);
        if (id) {
            strncpy(s_current_channel_id, id, sizeof(s_current_channel_id) - 1);
            s_current_channel_id[sizeof(s_current_channel_id) - 1] = '\0';
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
        s_channel_load_abort = true;
    }
}

esp_err_t makapix_request_channel_switch(const char *channel, const char *identifier)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build channel_id for comparison
    char new_channel_id[128] = {0};
    if (strcmp(channel, "by_user") == 0 && identifier) {
        snprintf(new_channel_id, sizeof(new_channel_id), "by_user_%s", identifier);
    } else if (strcmp(channel, "hashtag") == 0 && identifier) {
        snprintf(new_channel_id, sizeof(new_channel_id), "hashtag_%s", identifier);
    } else {
        strncpy(new_channel_id, channel, sizeof(new_channel_id) - 1);
    }
    
    // Check if this is the same channel already loading
    if (s_channel_loading && strcmp(s_loading_channel_id, new_channel_id) == 0) {
        return ESP_OK;
    }
    
    // Store as pending channel
    strncpy(s_pending_channel, channel, sizeof(s_pending_channel) - 1);
    s_pending_channel[sizeof(s_pending_channel) - 1] = '\0';
    
    if (identifier) {
        strncpy(s_pending_user_handle, identifier, sizeof(s_pending_user_handle) - 1);
        s_pending_user_handle[sizeof(s_pending_user_handle) - 1] = '\0';
    } else {
        s_pending_user_handle[0] = '\0';
    }
    
    s_has_pending_channel = true;
    
    if (s_channel_loading) {
        s_channel_load_abort = true;
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

void makapix_clear_current_channel(void)
{
    // Clear the current channel ID so we don't skip switching back to the same channel later
    // This should be called when switching away from Makapix (e.g., to SD card)
    s_current_channel_id[0] = '\0';
    s_current_channel = NULL;  // Don't destroy - ownership may have been transferred
    ESP_LOGD(MAKAPIX_TAG, "Cleared current Makapix channel state");
}

// ---------------------------------------------------------------------------
// Background channel index refresh (for Play Scheduler)
// ---------------------------------------------------------------------------

// Track background refresh handles to avoid recreating them repeatedly
static channel_handle_t s_refresh_handle_all = NULL;
static channel_handle_t s_refresh_handle_promoted = NULL;

// ---------------------------------------------------------------------------
// Play Scheduler refresh completion tracking
// ---------------------------------------------------------------------------

#define MAX_PS_PENDING_REFRESH 8

typedef struct {
    char channel_id[64];
    bool completed;
} ps_refresh_pending_t;

static ps_refresh_pending_t s_ps_pending_refreshes[MAX_PS_PENDING_REFRESH] = {0};
static SemaphoreHandle_t s_ps_pending_mutex = NULL;

void makapix_ps_refresh_register(const char *channel_id)
{
    if (!channel_id) return;

    // Create mutex on first use
    if (!s_ps_pending_mutex) {
        s_ps_pending_mutex = xSemaphoreCreateMutex();
        if (!s_ps_pending_mutex) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create PS refresh mutex");
            return;
        }
    }

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    // Check if already registered
    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0) {
            // Already registered, reset completion flag
            s_ps_pending_refreshes[i].completed = false;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGD(MAKAPIX_TAG, "PS refresh re-registered: %s", channel_id);
            return;
        }
    }

    // Find empty slot
    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (s_ps_pending_refreshes[i].channel_id[0] == '\0') {
            strlcpy(s_ps_pending_refreshes[i].channel_id, channel_id,
                    sizeof(s_ps_pending_refreshes[i].channel_id));
            s_ps_pending_refreshes[i].completed = false;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGD(MAKAPIX_TAG, "PS refresh registered: %s", channel_id);
            return;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    ESP_LOGW(MAKAPIX_TAG, "PS refresh table full, cannot register: %s", channel_id);
}

void makapix_ps_refresh_mark_complete(const char *channel_id)
{
    if (!channel_id || !s_ps_pending_mutex) return;

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0) {
            s_ps_pending_refreshes[i].completed = true;
            xSemaphoreGive(s_ps_pending_mutex);
            ESP_LOGI(MAKAPIX_TAG, "PS refresh complete: %s", channel_id);
            // Signal Play Scheduler
            makapix_channel_signal_ps_refresh_done(channel_id);
            return;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    // Not registered - that's OK, may have been triggered by non-PS path
}

bool makapix_ps_refresh_check_and_clear(const char *channel_id)
{
    if (!channel_id || !s_ps_pending_mutex) return false;

    xSemaphoreTake(s_ps_pending_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PS_PENDING_REFRESH; i++) {
        if (strcmp(s_ps_pending_refreshes[i].channel_id, channel_id) == 0 &&
            s_ps_pending_refreshes[i].completed) {
            // Clear the entry
            s_ps_pending_refreshes[i].channel_id[0] = '\0';
            s_ps_pending_refreshes[i].completed = false;
            xSemaphoreGive(s_ps_pending_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_ps_pending_mutex);
    return false;
}

// ---------------------------------------------------------------------------

esp_err_t makapix_refresh_channel_index(const char *channel_type, const char *identifier)
{
    if (!channel_type) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check MQTT connection
    if (!makapix_mqtt_is_connected()) {
        ESP_LOGW(MAKAPIX_TAG, "Cannot refresh channel: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    // Build channel_id from type and identifier
    char channel_id[128] = {0};
    char channel_name[64] = {0};

    if (strcmp(channel_type, "all") == 0) {
        strncpy(channel_id, "all", sizeof(channel_id) - 1);
        strncpy(channel_name, "All", sizeof(channel_name) - 1);
    } else if (strcmp(channel_type, "promoted") == 0) {
        strncpy(channel_id, "promoted", sizeof(channel_id) - 1);
        strncpy(channel_name, "Promoted", sizeof(channel_name) - 1);
    } else if (strcmp(channel_type, "by_user") == 0 && identifier) {
        snprintf(channel_id, sizeof(channel_id), "by_user_%s", identifier);
        snprintf(channel_name, sizeof(channel_name), "User %s", identifier);
    } else if (strcmp(channel_type, "hashtag") == 0 && identifier) {
        snprintf(channel_id, sizeof(channel_id), "hashtag_%s", identifier);
        snprintf(channel_name, sizeof(channel_name), "#%s", identifier);
    } else {
        ESP_LOGW(MAKAPIX_TAG, "Unknown channel type: %s", channel_type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(MAKAPIX_TAG, "Refreshing channel index: %s (no channel switch)", channel_id);

    // Register for Play Scheduler completion tracking
    makapix_ps_refresh_register(channel_id);

    // Get paths
    char vault_path[128] = {0};
    char channels_path[128] = {0};
    if (sd_path_get_vault(vault_path, sizeof(vault_path)) != ESP_OK) {
        snprintf(vault_path, sizeof(vault_path), "%s/vault", SD_PATH_DEFAULT_ROOT);
    }
    if (sd_path_get_channel(channels_path, sizeof(channels_path)) != ESP_OK) {
        snprintf(channels_path, sizeof(channels_path), "%s/channel", SD_PATH_DEFAULT_ROOT);
    }

    // Check if we already have a handle for this channel type (reuse for "all" and "promoted")
    channel_handle_t handle = NULL;
    if (strcmp(channel_type, "all") == 0) {
        if (!s_refresh_handle_all) {
            s_refresh_handle_all = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        }
        handle = s_refresh_handle_all;
    } else if (strcmp(channel_type, "promoted") == 0) {
        if (!s_refresh_handle_promoted) {
            s_refresh_handle_promoted = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        }
        handle = s_refresh_handle_promoted;
    } else {
        // For user/hashtag channels, create a temporary handle
        // (In practice, these are less common and can be created on demand)
        handle = makapix_channel_create(channel_id, channel_name, vault_path, channels_path);
        if (!handle) {
            ESP_LOGE(MAKAPIX_TAG, "Failed to create channel for refresh: %s", channel_id);
            return ESP_ERR_NO_MEM;
        }
        // Load to trigger the refresh task
        esp_err_t err = channel_load(handle);
        // For temporary handles, we don't keep them - they'll clean up when refresh completes
        // Note: The refresh task has its own reference and will complete in background
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(MAKAPIX_TAG, "Channel load/refresh failed: %s", esp_err_to_name(err));
            channel_destroy(handle);
            return err;
        }
        return ESP_OK;
    }

    if (!handle) {
        ESP_LOGE(MAKAPIX_TAG, "Failed to create/get channel handle for refresh: %s", channel_id);
        return ESP_ERR_NO_MEM;
    }

    // Trigger refresh via channel_load (which starts the refresh task if needed)
    esp_err_t err = channel_load(handle);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(MAKAPIX_TAG, "Channel load/refresh failed: %s", esp_err_to_name(err));
        return err;
    }

    // Additionally, explicitly request refresh if the channel is already loaded
    // This ensures the refresh task queries for new data even if cache already exists
    channel_request_refresh(handle);

    ESP_LOGD(MAKAPIX_TAG, "Refresh initiated for %s (background)", channel_id);
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

static esp_err_t storage_key_sha256_local(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) return ESP_ERR_INVALID_ARG;
    int ret = mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), out_sha256, 0);
    if (ret != 0) {
        ESP_LOGE(MAKAPIX_TAG, "SHA256 failed (ret=%d)", ret);
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
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        snprintf(out, out_len, "%s/vault/%s.webp", SD_PATH_DEFAULT_ROOT, storage_key);
        return;
    }
    
    uint8_t sha256[32];
    if (storage_key_sha256_local(storage_key, sha256) != ESP_OK) {
        snprintf(out, out_len, "%s/%s%s", vault_base, storage_key, ".webp");
        return;
    }
    char dir1[3], dir2[3], dir3[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
    snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);
    // Include file extension for type detection
    int ext_idx = detect_file_type_ext(art_url);
    snprintf(out, out_len, "%s/%s/%s/%s/%s%s", vault_base, dir1, dir2, dir3, storage_key, s_ext_strings_local[ext_idx]);
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
            ESP_LOGD(MAKAPIX_TAG, "Downloading artwork (attempt %d/%d)...", attempt, max_attempts);
            esp_err_t err = makapix_artwork_download(ch->art_url, ch->item.storage_key, ch->item.filepath, sizeof(ch->item.filepath));
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGW(MAKAPIX_TAG, "Download attempt %d failed: %s", attempt, esp_err_to_name(err));
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
