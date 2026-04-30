// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "p3a_reaction_dispatcher.h"
#include "p3a_current_post.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "p3a_reaction_dispatcher";

// Reaction overlay (from display_reaction_overlay.c via weak symbols)
extern void reaction_overlay_show_submit(void) __attribute__((weak));
extern void reaction_overlay_show_revoke(void) __attribute__((weak));
extern void reaction_overlay_show_error(void) __attribute__((weak));
extern void reaction_overlay_show_click(void) __attribute__((weak));

// Makapix API reaction functions (from makapix_api.c via weak symbols)
extern esp_err_t makapix_api_submit_reaction(int32_t post_id, const char *emoji) __attribute__((weak));
extern esp_err_t makapix_api_revoke_reaction(int32_t post_id, const char *emoji) __attribute__((weak));

// MQTT connection probe (from makapix_mqtt.c via weak symbol)
extern bool makapix_mqtt_is_connected(void) __attribute__((weak));

// Giphy click registration + config accessors (from giphy / config_store via weak symbols)
extern esp_err_t giphy_register_click(const char *api_key, const char *random_id,
                                      const char *giphy_id) __attribute__((weak));
extern esp_err_t config_store_get_giphy_api_key(char *out_key, size_t max_len) __attribute__((weak));
extern esp_err_t config_store_get_giphy_random_id(char *out, size_t max_len) __attribute__((weak));

// Thumbs-up emoji (UTF-8: U+1F44D).
static const char THUMBS_UP_EMOJI[] = "\xF0\x9F\x91\x8D";

// ---------------------------------------------------------------------------
// Makapix submit / revoke
// ---------------------------------------------------------------------------

typedef struct {
    int32_t post_id;
    bool    is_submit;
} makapix_reaction_params_t;

static void makapix_reaction_task(void *arg)
{
    makapix_reaction_params_t *p = (makapix_reaction_params_t *)arg;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (p->is_submit) {
        if (makapix_api_submit_reaction) {
            err = makapix_api_submit_reaction(p->post_id, THUMBS_UP_EMOJI);
        }
    } else {
        if (makapix_api_revoke_reaction) {
            err = makapix_api_revoke_reaction(p->post_id, THUMBS_UP_EMOJI);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reaction %s failed for post_id=%" PRId32 ": %s",
                 p->is_submit ? "submit" : "revoke", p->post_id, esp_err_to_name(err));
        if (reaction_overlay_show_error) {
            reaction_overlay_show_error();
        }
        // Revert optimistic flag only if the call still applies to the
        // currently-displayed post; otherwise the artwork has already changed
        // and p3a_current_post_set() has already reset the flag.
        if (p3a_current_post_get_id() == p->post_id) {
            p3a_current_post_set_reaction_submitted(!p->is_submit);
        }
    }
    free(p);
    vTaskDelete(NULL);
}

static esp_err_t dispatch_makapix(int32_t post_id, bool is_submit)
{
    if (post_id <= 0) return ESP_ERR_INVALID_ARG;
    if (!makapix_mqtt_is_connected || !makapix_mqtt_is_connected()) {
        ESP_LOGI(TAG, "Reaction %s requested while MQTT not connected",
                 is_submit ? "submit" : "revoke");
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_INVALID_STATE;
    }

    // Optimistic state update + overlay before spawning the task.
    p3a_current_post_set_reaction_submitted(is_submit);
    if (is_submit) {
        if (reaction_overlay_show_submit) reaction_overlay_show_submit();
    } else {
        if (reaction_overlay_show_revoke) reaction_overlay_show_revoke();
    }

    makapix_reaction_params_t *p = malloc(sizeof(*p));
    if (!p) {
        ESP_LOGE(TAG, "Failed to allocate reaction task params");
        // Revert the optimistic flag so the UI doesn't lie.
        p3a_current_post_set_reaction_submitted(!is_submit);
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    p->post_id = post_id;
    p->is_submit = is_submit;

    BaseType_t ret = xTaskCreate(makapix_reaction_task, "reaction_mqtt", 4096, p, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reaction MQTT task");
        free(p);
        p3a_current_post_set_reaction_submitted(!is_submit);
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Dispatched reaction %s: post_id=%" PRId32,
             is_submit ? "submit" : "revoke", post_id);
    return ESP_OK;
}

esp_err_t p3a_reaction_dispatch_makapix_submit(int32_t post_id)
{
    return dispatch_makapix(post_id, true);
}

esp_err_t p3a_reaction_dispatch_makapix_revoke(int32_t post_id)
{
    return dispatch_makapix(post_id, false);
}

// ---------------------------------------------------------------------------
// Giphy click
// ---------------------------------------------------------------------------

typedef struct {
    char api_key[128];
    char random_id[40];
    char giphy_id[24];
} giphy_click_params_t;

static void giphy_click_task(void *arg)
{
    giphy_click_params_t *p = (giphy_click_params_t *)arg;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (giphy_register_click) {
        err = giphy_register_click(p->api_key, p->random_id, p->giphy_id);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Giphy click failed for %s: %s", p->giphy_id, esp_err_to_name(err));
        if (reaction_overlay_show_error) {
            reaction_overlay_show_error();
        }
    } else {
        ESP_LOGI(TAG, "Giphy click registered: %s", p->giphy_id);
    }
    free(p);
    vTaskDelete(NULL);
}

esp_err_t p3a_reaction_dispatch_giphy_click(const char *giphy_id)
{
    if (!giphy_id || !giphy_id[0]) return ESP_ERR_INVALID_ARG;

    char api_key[128] = "";
    char random_id[40] = "";
    if (!config_store_get_giphy_api_key ||
        config_store_get_giphy_api_key(api_key, sizeof(api_key)) != ESP_OK ||
        api_key[0] == '\0') {
        ESP_LOGI(TAG, "Giphy click without API key");
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_INVALID_STATE;
    }
    if (!config_store_get_giphy_random_id ||
        config_store_get_giphy_random_id(random_id, sizeof(random_id)) != ESP_OK ||
        random_id[0] == '\0') {
        ESP_LOGI(TAG, "Giphy click without random_id");
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_INVALID_STATE;
    }

    if (reaction_overlay_show_click) reaction_overlay_show_click();

    giphy_click_params_t *p = malloc(sizeof(*p));
    if (!p) {
        ESP_LOGE(TAG, "Failed to allocate giphy click params");
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }
    strlcpy(p->api_key, api_key, sizeof(p->api_key));
    strlcpy(p->random_id, random_id, sizeof(p->random_id));
    strlcpy(p->giphy_id, giphy_id, sizeof(p->giphy_id));

    // Stack: HTTPS handshake + JSON parse can spike past 4 KB.
    BaseType_t ret = xTaskCreate(giphy_click_task, "giphy_click", 8192, p, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create giphy click task");
        free(p);
        if (reaction_overlay_show_error) reaction_overlay_show_error();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Dispatched Giphy click: %s", giphy_id);
    return ESP_OK;
}
