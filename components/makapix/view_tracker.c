// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file view_tracker.c
 * @brief View tracker implementation: dwell timing and MQTT view acknowledgment
 */

#include "view_tracker.h"
#include "makapix_mqtt.h"
#include "makapix_store.h"
#include "play_scheduler_types.h"
#include "config_store.h"

// Forward declaration to avoid circular dependency (makapix <-> play_scheduler)
esp_err_t play_scheduler_get_stats(ps_stats_t *out_stats);

// Weak extern: app_lcd lives in main/, not a component we can REQUIRE here.
// Returns true while any info screen / UI overlay is shown instead of artwork.
extern bool app_lcd_is_ui_mode(void) __attribute__((weak));
#include "makapix.h"
#include "sntp_sync.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "freertos/atomic.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <string.h>
#include <inttypes.h>

static const char *TAG = "view_tracker";

// Timer interval: 1 second
#define VIEW_TRACKER_TICK_MS 1000

// Task poll interval
#define VIEW_TRACKER_POLL_MS 500

// Timing thresholds
#define VIEW_TRIGGER_SECONDS 5
#define VIEW_RESET_SECONDS 30

// Pending swap info (set by render task, consumed by view tracker task)
typedef struct {
    volatile uint32_t pending;
    int32_t post_id;
    post_source_t post_source;
    char filepath[256];
    ps_channel_type_t channel_type;
    char channel_spec_name[33];
    char channel_identifier[33];
} pending_swap_t;

static pending_swap_t s_pending_swap = {0};

// State structure
typedef struct {
    bool initialized;
    TimerHandle_t timer;
    TaskHandle_t task;

    // Current tracking state
    int32_t current_post_id;
    post_source_t current_post_source;
    char current_filepath[256];
    bool is_intentional;

    // Channel the post was picked from (for view-event reporting)
    ps_channel_type_t current_channel_type;
    char current_channel_spec_name[33];
    char current_channel_identifier[33];

    // Timer state
    uint32_t elapsed_seconds;
    bool tracking_active;
} view_tracker_state_t;

static view_tracker_state_t s_state = {0};

// Pause ref-count. Multiple independent sources (playback pause, info-screen
// overlay, future contributors) can request a pause; the dwell timer is
// stopped on the 0->1 transition and restarted on the 1->0 transition. Updated
// atomically because pause/resume can be invoked from different tasks
// (playback service vs the view-tracker task itself).
static volatile int32_t s_pause_count = 0;

// Cached player_key presence — avoids NVS lookup on every swap
static bool s_has_player_key = false;

// PSRAM-backed stack for view tracker task
static StackType_t *s_view_tracker_stack = NULL;
static StaticTask_t s_view_tracker_task_buffer;

// Forward declarations
static void timer_callback(TimerHandle_t timer);
static void view_tracker_task(void *pvParameters);
static void process_swap_event(void);
static void send_view_event(void);
static const char *channel_name_for_view(ps_channel_type_t type, const char *spec_name);
static const char *get_intent_string(bool is_intentional);
static void on_registration_changed(const p3a_event_t *event, void *ctx);

static void on_registration_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    s_has_player_key = (event->payload.i32 != 0);
}

esp_err_t view_tracker_init(void)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "View tracker already initialized");
        return ESP_OK;
    }
    
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_pending_swap, 0, sizeof(s_pending_swap));

    s_has_player_key = makapix_store_has_player_key();
    event_bus_subscribe(P3A_EVENT_REGISTRATION_CHANGED, on_registration_changed, NULL);

    // Create dedicated task for processing (6KB stack - enough for MQTT/JSON/logging)
    // Use SPIRAM-backed stack to free internal RAM
    const size_t view_tracker_stack_size = 6144;
    if (!s_view_tracker_stack) {
        s_view_tracker_stack = heap_caps_malloc(view_tracker_stack_size * sizeof(StackType_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    bool task_created = false;
    if (s_view_tracker_stack) {
        s_state.task = xTaskCreateStatic(view_tracker_task, "view_tracker",
                                          view_tracker_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY,
                                          s_view_tracker_stack, &s_view_tracker_task_buffer);
        task_created = (s_state.task != NULL);
    }

    if (!task_created) {
        if (xTaskCreate(view_tracker_task, "view_tracker",
                        view_tracker_stack_size, NULL, CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_state.task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create view tracker task");
            return ESP_ERR_NO_MEM;
        }
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

void view_tracker_signal_swap(int32_t post_id, post_source_t post_source, const char *filepath,
                              ps_channel_type_t channel_type,
                              const char *channel_spec_name,
                              const char *channel_identifier)
{
    // Capture the swap info for the view tracker task to process. Capturing at
    // swap time (rather than re-reading at view-send time) ensures the view
    // event reports the channel this specific post came from, even if the
    // playset's stochastic selection picks a different channel before the
    // view fires.
    s_pending_swap.post_id = post_id;
    s_pending_swap.post_source = post_source;
    if (filepath) {
        strlcpy(s_pending_swap.filepath, filepath, sizeof(s_pending_swap.filepath));
    } else {
        s_pending_swap.filepath[0] = '\0';
    }
    s_pending_swap.channel_type = channel_type;
    strlcpy(s_pending_swap.channel_spec_name, channel_spec_name ? channel_spec_name : "",
            sizeof(s_pending_swap.channel_spec_name));
    strlcpy(s_pending_swap.channel_identifier, channel_identifier ? channel_identifier : "",
            sizeof(s_pending_swap.channel_identifier));
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
    s_state.current_post_source = POST_SOURCE_NONE;
    s_state.elapsed_seconds = 0;
    s_state.current_filepath[0] = '\0';
    s_state.current_channel_type = PS_CHANNEL_TYPE_NAMED;
    s_state.current_channel_spec_name[0] = '\0';
    s_state.current_channel_identifier[0] = '\0';
}

void view_tracker_pause(void)
{
    if (!s_state.initialized) {
        return;
    }

    int32_t new_count = __atomic_add_fetch(&s_pause_count, 1, __ATOMIC_SEQ_CST);

    // Stop the timer only on the 0->1 edge, and only if there's something to
    // track. Calls made while no artwork is active still count toward the
    // ref-count so the matching resume() balances correctly.
    if (new_count == 1 && s_state.tracking_active && s_state.timer) {
        xTimerStop(s_state.timer, 0);
        ESP_LOGD(TAG, "View tracking paused at %" PRIu32 "s (refcount=1)",
                 s_state.elapsed_seconds);
    }
}

void view_tracker_resume(void)
{
    if (!s_state.initialized) {
        return;
    }

    int32_t prev = __atomic_fetch_sub(&s_pause_count, 1, __ATOMIC_SEQ_CST);

    // Underflow guard: more resumes than pauses indicates a caller bug. Undo
    // the decrement and warn so the imbalance is visible in logs.
    if (prev <= 0) {
        __atomic_add_fetch(&s_pause_count, 1, __ATOMIC_SEQ_CST);
        ESP_LOGW(TAG, "view_tracker_resume() called without matching pause (count was %" PRId32 ")",
                 prev);
        return;
    }

    // Restart the timer only on the 1->0 edge.
    if (prev == 1 && s_state.tracking_active && s_state.timer) {
        xTimerStart(s_state.timer, 0);
        ESP_LOGD(TAG, "View tracking resumed at %" PRIu32 "s (refcount=0)",
                 s_state.elapsed_seconds);
    }
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

    // Seed with current UI-mode state so we don't fire a spurious pause on the
    // first iteration if an info screen happens to be up at init time.
    bool last_ui_mode = (app_lcd_is_ui_mode && app_lcd_is_ui_mode());

    while (1) {
        // Check for swap event (poll the atomic flag)
        uint32_t swap_was_pending = __atomic_exchange_n(&s_pending_swap.pending, 0, __ATOMIC_ACQUIRE);
        if (swap_was_pending == 1) {
            // A swap occurred - process it with our own stack
            process_swap_event();
        }

        // Edge-detect UI mode transitions and pause/resume the dwell timer so
        // time spent on info screens (USB MSC, provisioning, OTA, ...) does
        // not count toward view-event triggers.
        bool now_ui_mode = (app_lcd_is_ui_mode && app_lcd_is_ui_mode());
        if (now_ui_mode != last_ui_mode) {
            if (now_ui_mode) {
                view_tracker_pause();
            } else {
                view_tracker_resume();
            }
            last_ui_mode = now_ui_mode;
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
    post_source_t post_source = s_pending_swap.post_source;
    char filepath[256];
    strlcpy(filepath, s_pending_swap.filepath, sizeof(filepath));
    ps_channel_type_t channel_type = s_pending_swap.channel_type;
    char channel_spec_name[33];
    char channel_identifier[33];
    strlcpy(channel_spec_name, s_pending_swap.channel_spec_name, sizeof(channel_spec_name));
    strlcpy(channel_identifier, s_pending_swap.channel_identifier, sizeof(channel_identifier));

    // Only track views for Makapix artwork
    if (post_source != POST_SOURCE_MAKAPIX || post_id == 0) {
        ESP_LOGD(TAG, "Not a Makapix artwork (source=%d), stopping tracker", post_source);
        view_tracker_stop();
        return;
    }

    if (!s_has_player_key) {
        ESP_LOGD(TAG, "No player_key, skipping view tracking");
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
    s_state.current_post_source = post_source;
    s_state.is_intentional = is_intentional;
    s_state.current_channel_type = channel_type;
    strlcpy(s_state.current_channel_spec_name, channel_spec_name, sizeof(s_state.current_channel_spec_name));
    strlcpy(s_state.current_channel_identifier, channel_identifier, sizeof(s_state.current_channel_identifier));
    s_state.elapsed_seconds = 0;
    s_state.tracking_active = true;

    strlcpy(s_state.current_filepath, filepath, sizeof(s_state.current_filepath));

    // Restart timer. If a pause request is currently outstanding (info screen,
    // playback pause, ...), stop it again immediately — without this, a swap
    // arriving mid-pause would tick dwell time even though the timer is
    // supposed to be held.
    xTimerStop(s_state.timer, 0);
    xTimerStart(s_state.timer, 0);
    int32_t paused = __atomic_load_n(&s_pause_count, __ATOMIC_ACQUIRE);
    if (paused > 0) {
        xTimerStop(s_state.timer, 0);
        ESP_LOGD(TAG, "Swap during pause (refcount=%" PRId32 "); timer kept paused",
                 paused);
    }

    ESP_LOGD(TAG, "Started tracking post_id=%" PRId32 ", intent=%s",
             post_id, is_intentional ? "artwork" : "channel");
}

static void send_view_event(void)
{
    if (!s_state.tracking_active || s_state.current_post_source != POST_SOURCE_MAKAPIX || s_state.current_post_id == 0) {
        ESP_LOGW(TAG, "Cannot send view: invalid state");
        return;
    }

    // Suppress view events while an info screen (USB MSC, provisioning, OTA, ...)
    // is covering the artwork — the user is not actually viewing it right now.
    if (app_lcd_is_ui_mode && app_lcd_is_ui_mode()) {
        ESP_LOGD(TAG, "Suppressing view at %" PRIu32 "s: UI overlay active",
                 s_state.elapsed_seconds);
        return;
    }

    ESP_LOGD(TAG, "Sending view at %" PRIu32 " seconds", s_state.elapsed_seconds);

    // Gather metadata for view event
    char player_key[37] = {0};
    if (makapix_store_get_player_key(player_key, sizeof(player_key)) != ESP_OK) {
        // Registration vanished mid-tracking (e.g. unregister raced this view).
        // Drop the cached flag and stop tracking instead of erroring on every
        // view trigger until the next swap.
        ESP_LOGW(TAG, "player_key no longer available, stopping view tracking");
        s_has_player_key = false;
        view_tracker_stop();
        return;
    }

    // Derive legacy play_order value from the device's global pick_mode
    uint8_t play_order = 1;  // Default: created/date order
    ps_stats_t ps_stats;
    if (play_scheduler_get_stats(&ps_stats) == ESP_OK) {
        play_order = (ps_stats.pick_mode == PS_PICK_RANDOM) ? 2 : 1;
    }

    const char *channel_name = channel_name_for_view(s_state.current_channel_type,
                                                     s_state.current_channel_spec_name);
    const char *intent = get_intent_string(s_state.is_intentional);

    // Determine channel-specific fields based on channel type
    const char *channel_user_sqid = NULL;
    const char *channel_hashtag = NULL;

    if ((s_state.current_channel_type == PS_CHANNEL_TYPE_USER ||
         s_state.current_channel_type == PS_CHANNEL_TYPE_REACTIONS) &&
        s_state.current_channel_identifier[0] != '\0') {
        channel_user_sqid = s_state.current_channel_identifier;
    } else if (s_state.current_channel_type == PS_CHANNEL_TYPE_HASHTAG &&
               s_state.current_channel_identifier[0] != '\0') {
        channel_hashtag = s_state.current_channel_identifier;
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
        ESP_LOGD(TAG, "Failed to send view event: %s", esp_err_to_name(err));
    }
}

// Server-facing channel name. PS_CHANNEL_TYPE_NAMED splits into "all" /
// "promoted" via spec_name; the rest map by channel type. Giphy is included
// for completeness but Giphy posts don't generate Makapix view events.
static const char *channel_name_for_view(ps_channel_type_t type, const char *spec_name)
{
    switch (type) {
        case PS_CHANNEL_TYPE_NAMED:
            if (spec_name && spec_name[0]) {
                return spec_name;  // "all", "promoted", ...
            }
            return "named";
        case PS_CHANNEL_TYPE_USER:      return "by_user";  // server name
        case PS_CHANNEL_TYPE_HASHTAG:   return "hashtag";
        case PS_CHANNEL_TYPE_SDCARD:    return "sdcard";
        case PS_CHANNEL_TYPE_ARTWORK:   return "artwork";
        case PS_CHANNEL_TYPE_GIPHY:     return "giphy";
        case PS_CHANNEL_TYPE_REACTIONS: return "reactions";
        case PS_CHANNEL_TYPE_INSTITUTION: return "institution";
        case PS_CHANNEL_TYPE_PINNED:    return "pinned";
    }
    return "unknown";
}

static const char *get_intent_string(bool is_intentional)
{
    // is_intentional = true means show_artwork command -> intent = "artwork"
    // is_intentional = false means channel playback -> intent = "channel"
    return is_intentional ? "artwork" : "channel";
}
