// SPDX-License-Identifier: Apache-2.0
// A registered player's questions over HTTPS. See makapix_rpc.h.

#include "makapix_rpc.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "http_fetch.h"
#include "makapix_parse.h"
#include "makapix_store.h"
#include "p3a_err.h"

static const char *TAG = "makapix";

#define RPC_URL "https://" MAKAPIX_HOST "/api/player/rpc"

static SemaphoreHandle_t s_lock;

esp_err_t makapix_rpc_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

const char *makapix_user_agent(void)
{
    static char agent[48];
    if (agent[0] == '\0') {
        snprintf(agent, sizeof(agent), "p3a/%s", esp_app_get_description()->version);
    }
    return agent;
}

// A new token from the server, saved. Rotation revokes the one before, so
// the new one is written before it is used: a token that was asked for and
// then lost would leave the device with a dead one (which the next 401
// repairs, at the cost of this exchange again).
static esp_err_t rotate_token(const char *player_key, char token[MAKAPIX_TOKEN_LEN])
{
    char url[160], answer[512];
    snprintf(url, sizeof(url), "https://" MAKAPIX_HOST "/api/player/%s/token/rotate", player_key);
    const http_fetch_request_t req = {
        .url = url, .method = HTTP_FETCH_POST, .user_agent = makapix_user_agent(), .max_attempts = 1};
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&req, answer, sizeof(answer), &got, NULL);
    if (err == ESP_OK) {
        err = makapix_parse_token(answer, token, MAKAPIX_TOKEN_LEN);
    }
    memset(answer, 0, sizeof(answer));  // it held the token
    if (err == ESP_OK) {
        const esp_err_t saved = makapix_store_set_token(token);
        if (saved != ESP_OK) {
            ESP_LOGW(TAG, "the new token could not be saved (%s); used from memory", p3a_err_to_name(saved));
        }
        ESP_LOGI(TAG, "the player has a new token");
    } else {
        ESP_LOGW(TAG, "no token from the server: %s", p3a_err_to_name(err));
    }
    return err;
}

static esp_err_t post(const char *body, const char *token, char *out, size_t out_len, bool (*stop)(void *), void *stop_ctx)
{
    char bearer[MAKAPIX_TOKEN_LEN + 8];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    const http_fetch_header_t headers[] = {{"Authorization", bearer}, {"Content-Type", "application/json"}};
    const http_fetch_request_t req = {.url = RPC_URL,
                                      .method = HTTP_FETCH_POST,
                                      .body = body,
                                      .body_len = strlen(body),
                                      .headers = headers,
                                      .header_count = 2,
                                      .user_agent = makapix_user_agent(),
                                      .should_abort = stop,
                                      .user_ctx = stop_ctx};
    size_t got = 0;
    const esp_err_t err = http_fetch_to_buffer(&req, out, out_len, &got, NULL);
    memset(bearer, 0, sizeof(bearer));
    return err;
}

esp_err_t makapix_rpc(const char *body, char *out, size_t out_len, bool (*stop)(void *), void *stop_ctx)
{
    makapix_identity_t identity;
    if (s_lock == NULL || makapix_store_identity(&identity) != ESP_OK) {
        return P3A_ERR_NO_KEY;
    }
    // One exchange at a time: two tasks finding the token dead at once
    // would each rotate it, and the second rotation kills the first's.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char token[MAKAPIX_TOKEN_LEN];
    esp_err_t err = makapix_store_token(token) ? ESP_OK : rotate_token(identity.player_key, token);
    if (err == ESP_OK) {
        err = post(body, token, out, out_len, stop, stop_ctx);
        if (err == (esp_err_t)P3A_ERR_AUTH) {
            // The server does not know this token (it was rotated from the
            // site, or a rotation's answer was lost). One new one, one more try.
            err = rotate_token(identity.player_key, token);
            if (err == ESP_OK) {
                err = post(body, token, out, out_len, stop, stop_ctx);
            } else if (err == (esp_err_t)P3A_ERR_MISSING) {
                err = P3A_ERR_AUTH;  // the server has no such player: the registration is gone
            }
        }
    } else if (err == (esp_err_t)P3A_ERR_MISSING) {
        err = P3A_ERR_AUTH;
    }
    memset(token, 0, sizeof(token));
    xSemaphoreGive(s_lock);
    return err;
}
