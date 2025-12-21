/**
 * @file makapix_internal.h
 * @brief Internal declarations for makapix component split files
 */

#pragma once

#include "makapix.h"
#include "makapix_store.h"
#include "makapix_provision.h"
#include "makapix_mqtt.h"
#include "makapix_api.h"
#include "makapix_channel_impl.h"
#include "makapix_channel_events.h"
#include "makapix_artwork.h"
#include "download_manager.h"
#include "channel_interface.h"
#include "sdio_bus.h"
#include "app_wifi.h"
#include "p3a_state.h"
#include "p3a_render.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>

// Forward declarations (to avoid including headers with dependencies not available to this component)
esp_err_t app_lcd_enter_ui_mode(void);
void app_lcd_exit_ui_mode(void);
esp_err_t ugfx_ui_show_channel_message(const char *channel_name, const char *message, int progress_percent);
void ugfx_ui_hide_channel_message(void);
esp_err_t channel_player_switch_to_makapix_channel(channel_handle_t makapix_channel);
esp_err_t channel_player_switch_to_sdcard_channel(void);
esp_err_t animation_player_request_swap_current(void);
void channel_player_clear_channel(channel_handle_t channel_to_clear);

// Shared TAG for logging
extern const char *MAKAPIX_TAG;

// Status publish interval
#define STATUS_PUBLISH_INTERVAL_MS (30000)

// --------------------------------------------------------------------------
// Shared state (defined in makapix.c, accessed by other split files)
// --------------------------------------------------------------------------

// State machine
extern makapix_state_t s_makapix_state;
extern int32_t s_current_post_id;
extern bool s_view_intent_intentional;

// Registration/provisioning
extern char s_registration_code[8];
extern char s_registration_expires[64];
extern char s_provisioning_status[128];
extern bool s_provisioning_cancelled;

// Task handles
extern TaskHandle_t s_poll_task_handle;
extern TaskHandle_t s_reconnect_task_handle;
extern TaskHandle_t s_status_publish_task_handle;
extern TaskHandle_t s_channel_switch_task_handle;

// Timer
extern TimerHandle_t s_status_timer;

// Channel state
extern channel_handle_t s_current_channel;
extern volatile bool s_channel_loading;
extern volatile bool s_channel_load_abort;
extern char s_loading_channel_id[128];
extern char s_current_channel_id[128];
extern char s_previous_channel_id[128];

// Pending channel request
extern char s_pending_channel[64];
extern char s_pending_user_handle[64];
extern volatile bool s_has_pending_channel;
extern SemaphoreHandle_t s_channel_switch_sem;

// --------------------------------------------------------------------------
// Internal functions (provisioning flow)
// --------------------------------------------------------------------------

/**
 * @brief Provisioning task entry point
 */
void makapix_provisioning_task(void *pvParameters);

/**
 * @brief Credentials polling task entry point
 */
void makapix_credentials_poll_task(void *pvParameters);

// --------------------------------------------------------------------------
// Internal functions (connection management)
// --------------------------------------------------------------------------

/**
 * @brief MQTT connection state change callback
 */
void makapix_mqtt_connection_callback(bool connected);

/**
 * @brief MQTT reconnection task entry point
 */
void makapix_mqtt_reconnect_task(void *pvParameters);

/**
 * @brief Status timer callback
 */
void makapix_status_timer_callback(TimerHandle_t xTimer);

/**
 * @brief Status publish task entry point
 */
void makapix_status_publish_task(void *pvParameters);

/**
 * @brief Channel switch task entry point
 */
void makapix_channel_switch_task(void *pvParameters);

