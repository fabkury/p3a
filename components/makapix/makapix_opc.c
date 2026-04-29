// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_opc.c
 * @brief Optional Player Commands (OPC) implementation
 */

#include "makapix_opc.h"
#include "makapix_mqtt.h"
#include "makapix_store.h"
#include "event_bus.h"
#include "playback_service.h"
#include "app_lcd.h"
#include "animation_player.h"
#include "version.h"
#include "esp_err.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "makapix_opc";

// Brightness range advertised in capabilities
#define OPC_BRIGHTNESS_MIN  0
#define OPC_BRIGHTNESS_MAX  100
#define OPC_BRIGHTNESS_STEP 1

// User-intended brightness; tracks the value the user last requested.
// While paused, the hardware backlight is at 0 but s_user_brightness keeps
// the pre-pause value so the server-side UI keeps showing the same number.
static int s_user_brightness = OPC_BRIGHTNESS_MAX;
static bool s_is_paused = false;
static bool s_initialized = false;

// ---------- Topic helpers ----------

static esp_err_t build_topic(const char *suffix, char *out, size_t max_len)
{
    char player_key[37];
    if (makapix_store_get_player_key(player_key, sizeof(player_key)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    int n = snprintf(out, max_len, "makapix/player/%s/%s", player_key, suffix);
    return (n > 0 && (size_t)n < max_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

// ---------- Publishing helpers ----------

static esp_err_t publish_state_obj(cJSON *state)
{
    char topic[160];
    esp_err_t err = build_topic("state", topic, sizeof(topic));
    if (err != ESP_OK) {
        cJSON_Delete(state);
        return err;
    }

    char *json = cJSON_PrintUnformatted(state);
    cJSON_Delete(state);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    err = makapix_mqtt_publish_with_retain(topic, json, 1, true);
    free(json);
    return err;
}

static void publish_state_pause_field(void)
{
    cJSON *state = cJSON_CreateObject();
    if (!state) return;
    cJSON_AddBoolToObject(state, "is_paused", s_is_paused);
    publish_state_obj(state);
}

static void publish_state_brightness_field(void)
{
    cJSON *state = cJSON_CreateObject();
    if (!state) return;
    cJSON_AddNumberToObject(state, "brightness", (double)s_user_brightness);
    publish_state_obj(state);
}

static void publish_state_rotation_field(int rotation)
{
    cJSON *state = cJSON_CreateObject();
    if (!state) return;
    cJSON_AddNumberToObject(state, "rotation", (double)rotation);
    publish_state_obj(state);
}

static esp_err_t publish_ack(const char *command_id, const char *status, const char *error)
{
    if (!command_id) {
        // Without an id the server cannot correlate the ack — log and skip.
        ESP_LOGW(TAG, "Cannot ack: missing command_id (status=%s)", status);
        return ESP_ERR_INVALID_ARG;
    }

    char topic[160];
    esp_err_t err = build_topic("command/ack", topic, sizeof(topic));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "command_id", command_id);
    cJSON_AddStringToObject(root, "status", status);
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    } else {
        cJSON_AddNullToObject(root, "error");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    err = makapix_mqtt_publish_raw(topic, json, 1);
    free(json);
    return err;
}

// ---------- Event-bus subscribers ----------

static void on_pause(const p3a_event_t *event, void *ctx)
{
    (void)event; (void)ctx;
    if (s_is_paused) return;
    s_is_paused = true;
    ESP_LOGD(TAG, "is_paused -> true");
    publish_state_pause_field();
}

static void on_resume(const p3a_event_t *event, void *ctx)
{
    (void)event; (void)ctx;
    if (!s_is_paused) return;
    s_is_paused = false;
    ESP_LOGD(TAG, "is_paused -> false");
    publish_state_pause_field();
}

static void on_brightness_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    int new_brightness = event->payload.i32;
    if (new_brightness < OPC_BRIGHTNESS_MIN) new_brightness = OPC_BRIGHTNESS_MIN;
    if (new_brightness > OPC_BRIGHTNESS_MAX) new_brightness = OPC_BRIGHTNESS_MAX;

    // While paused, brightness is forced to 0 (and restored on resume) by
    // playback_service. Those transitions are not user-intent changes, so do
    // not update the advertised state.
    if (s_is_paused) {
        return;
    }

    if (new_brightness == s_user_brightness) {
        return;
    }
    s_user_brightness = new_brightness;
    ESP_LOGD(TAG, "brightness -> %d", s_user_brightness);
    publish_state_brightness_field();
}

static void on_rotation_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    int rotation = event->payload.i32;
    ESP_LOGD(TAG, "rotation -> %d", rotation);
    publish_state_rotation_field(rotation);
}

// ---------- Public API ----------

esp_err_t makapix_opc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_user_brightness = app_lcd_get_brightness();
    if (s_user_brightness < OPC_BRIGHTNESS_MIN) s_user_brightness = OPC_BRIGHTNESS_MIN;
    if (s_user_brightness > OPC_BRIGHTNESS_MAX) s_user_brightness = OPC_BRIGHTNESS_MAX;
    s_is_paused = playback_service_is_paused();

    event_bus_subscribe(P3A_EVENT_PAUSE, on_pause, NULL);
    event_bus_subscribe(P3A_EVENT_RESUME, on_resume, NULL);
    event_bus_subscribe(P3A_EVENT_BRIGHTNESS_CHANGED, on_brightness_changed, NULL);
    event_bus_subscribe(P3A_EVENT_ROTATION_CHANGED, on_rotation_changed, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized (brightness=%d, paused=%d)", s_user_brightness, s_is_paused);
    return ESP_OK;
}

esp_err_t makapix_opc_publish_capabilities(void)
{
    char topic[160];
    esp_err_t err = build_topic("capabilities", topic, sizeof(topic));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "firmware_version", FW_VERSION);

    cJSON *features = cJSON_AddObjectToObject(root, "features");
    if (!features) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    // pause: empty object (no parameters)
    cJSON_AddItemToObject(features, "pause", cJSON_CreateObject());

    // brightness: { min, max, step }
    cJSON *brightness = cJSON_AddObjectToObject(features, "brightness");
    cJSON_AddNumberToObject(brightness, "min", OPC_BRIGHTNESS_MIN);
    cJSON_AddNumberToObject(brightness, "max", OPC_BRIGHTNESS_MAX);
    cJSON_AddNumberToObject(brightness, "step", OPC_BRIGHTNESS_STEP);

    // rotation: { values: [0, 90, 180, 270] }
    cJSON *rotation = cJSON_AddObjectToObject(features, "rotation");
    cJSON *values = cJSON_AddArrayToObject(rotation, "values");
    cJSON_AddItemToArray(values, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(values, cJSON_CreateNumber(90));
    cJSON_AddItemToArray(values, cJSON_CreateNumber(180));
    cJSON_AddItemToArray(values, cJSON_CreateNumber(270));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    err = makapix_mqtt_publish_with_retain(topic, json, 1, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Published capabilities (fw %s)", FW_VERSION);
    } else {
        ESP_LOGW(TAG, "Failed to publish capabilities: %s", esp_err_to_name(err));
    }
    free(json);
    return err;
}

esp_err_t makapix_opc_publish_state_full(void)
{
    cJSON *state = cJSON_CreateObject();
    if (!state) return ESP_ERR_NO_MEM;

    cJSON_AddBoolToObject(state, "is_paused", s_is_paused);
    cJSON_AddNumberToObject(state, "brightness", (double)s_user_brightness);
    cJSON_AddNumberToObject(state, "rotation", (double)app_get_screen_rotation());

    esp_err_t err = publish_state_obj(state);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Published initial state");
    } else {
        ESP_LOGW(TAG, "Failed to publish initial state: %s", esp_err_to_name(err));
    }
    return err;
}

// ---------- Command handler ----------

static bool handle_set_paused(const cJSON *payload, const char *command_id)
{
    cJSON *paused = payload ? cJSON_GetObjectItem((cJSON *)payload, "paused") : NULL;
    if (!paused || !cJSON_IsBool(paused)) {
        publish_ack(command_id, "error", "missing or invalid 'paused' field");
        return true;
    }

    bool want_paused = cJSON_IsTrue(paused);

    // Apply via event bus, matching the existing REST /action/pause flow.
    // The actual hardware change happens in the playback-event subscriber in
    // p3a_main.c, and our own subscribers will pick up the resulting state
    // change and publish it.
    if (want_paused != s_is_paused) {
        esp_err_t err = event_bus_emit_simple(want_paused ? P3A_EVENT_PAUSE : P3A_EVENT_RESUME);
        if (err != ESP_OK) {
            publish_ack(command_id, "error", esp_err_to_name(err));
            return true;
        }
    }

    publish_ack(command_id, "ok", NULL);
    return true;
}

static bool handle_set_brightness(const cJSON *payload, const char *command_id)
{
    cJSON *value = payload ? cJSON_GetObjectItem((cJSON *)payload, "value") : NULL;
    if (!value || !cJSON_IsNumber(value)) {
        publish_ack(command_id, "error", "missing or invalid 'value' field");
        return true;
    }

    int v = (int)cJSON_GetNumberValue(value);
    if (v < OPC_BRIGHTNESS_MIN) v = OPC_BRIGHTNESS_MIN;
    if (v > OPC_BRIGHTNESS_MAX) v = OPC_BRIGHTNESS_MAX;

    esp_err_t err = app_lcd_set_brightness(v);
    if (err != ESP_OK) {
        publish_ack(command_id, "error", esp_err_to_name(err));
        return true;
    }

    publish_ack(command_id, "ok", NULL);
    return true;
}

static bool handle_set_rotation(const cJSON *payload, const char *command_id)
{
    cJSON *value = payload ? cJSON_GetObjectItem((cJSON *)payload, "value") : NULL;
    if (!value || !cJSON_IsNumber(value)) {
        publish_ack(command_id, "error", "missing or invalid 'value' field");
        return true;
    }

    int v = (int)cJSON_GetNumberValue(value);
    if (v != 0 && v != 90 && v != 180 && v != 270) {
        publish_ack(command_id, "error", "rotation must be 0, 90, 180, or 270");
        return true;
    }

    esp_err_t err = app_set_screen_rotation((screen_rotation_t)v);
    if (err == ESP_ERR_INVALID_STATE) {
        publish_ack(command_id, "error", "rotation operation already in progress");
        return true;
    }
    if (err != ESP_OK) {
        publish_ack(command_id, "error", esp_err_to_name(err));
        return true;
    }

    publish_ack(command_id, "ok", NULL);
    return true;
}

bool makapix_opc_handle_command(const char *command_type, const cJSON *payload,
                                const char *command_id)
{
    if (!command_type) return false;

    if (strcmp(command_type, "set_paused") == 0) {
        return handle_set_paused(payload, command_id);
    }
    if (strcmp(command_type, "set_brightness") == 0) {
        return handle_set_brightness(payload, command_id);
    }
    if (strcmp(command_type, "set_rotation") == 0) {
        return handle_set_rotation(payload, command_id);
    }
    if (strcmp(command_type, "set_mirror") == 0) {
        // Not advertised in capabilities; ack defensively in case the server
        // forwards one anyway.
        publish_ack(command_id, "unsupported", NULL);
        return true;
    }

    return false;
}
