// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "view_tracker.h"
#include "makapix_mqtt.h"
#include "makapix_store.h"
#include "config_store.h"
#include "p3a_state.h"
#include "makapix.h"
#include "sntp_sync.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/atomic.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "view_tracker";

// Timer interval: 1 second
#define VIEW_TRACKER_TICK_MS 1000

// Task poll interval
#define VIEW_TRACKER_POLL_MS 50

// Timing thresholds
#define VIEW_TRIGGER_SECONDS 5
#define VIEW_RESET_SECONDS 30

// Pending swap info (set by render task, consumed by view tracker task)
typedef struct {
    volatile uint32_t pending;
    int32_t post_id;
    char filepath[256];
} pending_swap_t;

static pending_swap_t s_pending_swap = {0};

// State structure
typedef struct {
    bool initialized;
    TimerHandle_t timer;
    TaskHandle_t task;
    
    // Current tracking state
    int32_t current_post_id;
    char current_filepath[256];
    bool is_intentional;
    
    // Timer state
    uint32_t elapsed_seconds;
    bool tracking_active;
} view_tracker_state_t;

static view_tracker_state_t s_state = {0};

// Forward declarations
static void timer_callback(TimerHandle_t timer);
static void view_tracker_task(void *pvParameters);
static void process_swap_event(void);
static void send_view_event(void);
static const char *get_channel_name_for_view(p3a_channel_type_t channel_type);
static const char *get_intent_string(bool is_intentional);

esp_err_t view_tracker_init(void)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "View tracker already initialized");
        return ESP_OK;
    }
    
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_pending_swap, 0, sizeof(s_pending_swap));
    
    // Create dedicated task for processing (6KB stack - enough for MQTT/JSON/logging)
    BaseType_t task_created = xTaskCreate(
        view_tracker_task,
        "view_tracker",
        6144,
        NULL,
        5,
        &s_state.task
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create view tracker task");
        return ESP_ERR_NO_MEM;
    }
    
    // Create FreeRTOS timer (auto-reload, 1-second period)
    s_state.timer = xTimerCreate(
        "view_timer",
        pdMS_TO_TICKS(VIEW_TRACKER_TICK_MS),
        pdTRUE,  // Auto-reload
        NULL,    // Timer ID (unused)
        timer_callback
    );
    
    if (!s_state.timer) {
        ESP_LOGE(TAG, "Failed to create timer");
        vTaskDelete(s_state.task);
        s_state.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    s_state.initialized = true;
    ESP_LOGI(TAG, "View tracker initialized");
    
    return ESP_OK;
}

void view_tracker_deinit(void)
{
    if (!s_state.initialized) {
        return;
    }
    
    if (s_state.timer) {
        xTimerStop(s_state.timer, 0);
        xTimerDelete(s_state.timer, 0);
        s_state.timer = NULL;
    }
    
    if (s_state.task) {
        vTaskDelete(s_state.task);
        s_state.task = NULL;
    }
    
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "View tracker deinitialized");
}

void view_tracker_signal_swap(int32_t post_id, const char *filepath)
{
    // Store the swap info for the view tracker task to process
    // This captures the post_id and filepath at swap time, before the navigator can advance
    s_pending_swap.post_id = post_id;
    if (filepath) {
        strlcpy(s_pending_swap.filepath, filepath, sizeof(s_pending_swap.filepath));
    } else {
        s_pending_swap.filepath[0] = '\0';
    }
    // Signal that a swap is pending (memory barrier to ensure data is visible)
    __atomic_store_n(&s_pending_swap.pending, 1, __ATOMIC_RELEASE);
}

void view_tracker_stop(void)
{
    if (!s_state.initialized) {
        return;
    }
    
    if (s_state.timer) {
        xTimerStop(s_state.timer, 0);
    }
    
    s_state.tracking_active = false;
    s_state.current_post_id = 0;
    s_state.elapsed_seconds = 0;
    s_state.current_filepath[0] = '\0';
}

static void timer_callback(TimerHandle_t timer)
{
    (void)timer;
    
    if (!s_state.tracking_active) {
        return;
    }
    
    s_state.elapsed_seconds++;
    
    // Check if we should trigger a view event
    // First event at 5s, then every 30s after that (35s, 65s, 95s, etc.)
    bool should_send = false;
    
    if (s_state.elapsed_seconds == VIEW_TRIGGER_SECONDS) {
        // First view at 5 seconds
        should_send = true;
    } else if (s_state.elapsed_seconds > VIEW_TRIGGER_SECONDS) {
        // Subsequent views every 30 seconds after the first 5
        // This happens at: 35s, 65s, 95s, etc.
        uint32_t since_first_view = s_state.elapsed_seconds - VIEW_TRIGGER_SECONDS;
        if (since_first_view % VIEW_RESET_SECONDS == 0) {
            should_send = true;
        }
    }
    
    if (should_send && s_state.task) {
        // Signal the task to send the view event
        xTaskNotifyGive(s_state.task);
    }
}

static void view_tracker_task(void *pvParameters)
{
    (void)pvParameters;
    
    while (1) {
        // Check for swap event (poll the atomic flag)
        uint32_t swap_was_pending = __atomic_exchange_n(&s_pending_swap.pending, 0, __ATOMIC_ACQUIRE);
        if (swap_was_pending == 1) {
            // A swap occurred - process it with our own stack
            process_swap_event();
        }
        
        // Check for view send notification from timer
        uint32_t notification = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(VIEW_TRACKER_POLL_MS));
        if (notification > 0) {
            send_view_event();
        }
    }
}

static void process_swap_event(void)
{
    // Read the swap info that was captured at swap time
    int32_t post_id = s_pending_swap.post_id;
    char filepath[256];
    strlcpy(filepath, s_pending_swap.filepath, sizeof(filepath));
    
    // Check for valid post_id
    if (post_id <= 0) {
        ESP_LOGD(TAG, "No valid post_id for swapped artwork");
        view_tracker_stop();
        return;
    }
    
    // Check if this is a Makapix artwork (filepath contains /vault/)
    if (strstr(filepath, "/vault/") == NULL) {
        ESP_LOGD(TAG, "Not a Makapix artwork, stopping tracker");
        view_tracker_stop();
        return;
    }
    
    // Get intent
    bool is_intentional = makapix_get_and_clear_view_intent();
    
    // Check if this is a redundant change (same artwork)
    if (s_state.tracking_active && 
        s_state.current_post_id == post_id &&
        strcmp(s_state.current_filepath, filepath) == 0) {
        ESP_LOGD(TAG, "Redundant animation change detected, not resetting timer");
        return;
    }
    
    // New animation - update state and restart timer
    s_state.current_post_id = post_id;
    s_state.is_intentional = is_intentional;
    s_state.elapsed_seconds = 0;
    s_state.tracking_active = true;
    
    strlcpy(s_state.current_filepath, filepath, sizeof(s_state.current_filepath));
    
    // Restart timer
    xTimerStop(s_state.timer, 0);
    xTimerStart(s_state.timer, 0);
    
    ESP_LOGI(TAG, "Started tracking post_id=%" PRId32 ", intent=%s", 
             post_id, is_intentional ? "artwork" : "channel");
}

static void send_view_event(void)
{
    if (!s_state.tracking_active || s_state.current_post_id <= 0) {
        ESP_LOGW(TAG, "Cannot send view: invalid state");
        return;
    }
    
    ESP_LOGD(TAG, "Sending view at %" PRIu32 " seconds", s_state.elapsed_seconds);
    
    // Gather metadata for view event
    char player_key[37] = {0};
    if (makapix_store_get_player_key(player_key, sizeof(player_key)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get player_key, cannot send view");
        return;
    }
    
    uint8_t play_order = config_store_get_play_order();
    
    p3a_channel_info_t channel_info = {0};
    if (p3a_state_get_channel_info(&channel_info) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get channel info");
        // Continue anyway with empty channel name
    }
    
    const char *channel_name = get_channel_name_for_view(channel_info.type);
    const char *intent = get_intent_string(s_state.is_intentional);
    
    // Determine channel-specific fields based on channel type
    const char *channel_user_sqid = NULL;
    const char *channel_hashtag = NULL;
    
    if (channel_info.type == P3A_CHANNEL_MAKAPIX_BY_USER && channel_info.identifier[0] != '\0') {
        channel_user_sqid = channel_info.identifier;
    } else if (channel_info.type == P3A_CHANNEL_MAKAPIX_HASHTAG && channel_info.identifier[0] != '\0') {
        channel_hashtag = channel_info.identifier;
    }
    
    // Get view acknowledgment setting
    bool request_ack = config_store_get_view_ack();
    
    // Send view event via MQTT
    esp_err_t err = makapix_mqtt_publish_view(
        s_state.current_post_id,
        intent,
        play_order,
        channel_name,
        player_key,
        channel_user_sqid,
        channel_hashtag,
        request_ack
    );
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "View event sent: post_id=%" PRId32 ", intent=%s, channel=%s, play_order=%u, ack=%s",
                 s_state.current_post_id, intent, channel_name, play_order, request_ack ? "true" : "false");
    } else {
        ESP_LOGW(TAG, "Failed to send view event: %s", esp_err_to_name(err));
    }
}

static const char *get_channel_name_for_view(p3a_channel_type_t channel_type)
{
    switch (channel_type) {
        case P3A_CHANNEL_SDCARD:
            return "sdcard";
        case P3A_CHANNEL_MAKAPIX_ALL:
            return "all";
        case P3A_CHANNEL_MAKAPIX_PROMOTED:
            return "promoted";
        case P3A_CHANNEL_MAKAPIX_USER:
            return "by_user";  // Server calls this "by_user"
        case P3A_CHANNEL_MAKAPIX_BY_USER:
            return "by_user";
        case P3A_CHANNEL_MAKAPIX_HASHTAG:
            return "hashtag";
        case P3A_CHANNEL_MAKAPIX_ARTWORK:
            return "artwork";
        default:
            return "unknown";
    }
}

static const char *get_intent_string(bool is_intentional)
{
    // is_intentional = true means show_artwork command -> intent = "artwork"
    // is_intentional = false means channel playback -> intent = "channel"
    return is_intentional ? "artwork" : "channel";
}
