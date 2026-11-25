// SPDX-License-Identifier: Apache-2.0
// Registering this device as a Makapix Club player, and the one task that
// does what Makapix needs done over time. See source_makapix.h.
//
// The steps (makapix_parse.h says what the answers hold):
//   1. Ask for a code and show it.
//   2. Every three seconds ask for the credentials; "not yet" (404) until
//      the owner has typed the code at makapix.club.
//   3. Save them. Only now does anything reach NVS: a registration that is
//      cancelled, or whose code runs out, leaves whatever registration the
//      device had exactly as it was.
//
// One task, started at boot and never ended, runs this; 1.x made a task
// for the first step and another for the second, on static stacks that a
// second start could hand to two tasks at once.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "makapix_internal.h"
#include "makapix_parse.h"
#include "makapix_rpc.h"
#include "makapix_store.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_mem.h"
#include "p3a_task.h"
#include "p3a_ui_hooks.h"
#include "psram_alloc.h"
#include "source_makapix.h"

static const char *TAG = "makapix";

#define POLL_MS            3000
#define CODE_WINDOW_S      900   // what the server gives a code, when its own word cannot be used
#define CODE_WINDOW_MAX_S  3600
#define DONE_SCREEN_MS     10000
#define CREDENTIALS_BYTES  (32 * 1024)

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static atomic_bool s_want_register, s_cancel;

// Guarded by s_lock.
static struct {
    source_makapix_phase_t phase;
    char code[MAKAPIX_CODE_LEN];
    int64_t deadline_ms;  // of the code, on the uptime clock
    char last_result[SOURCE_MAKAPIX_RESULT_LEN];
} s_state;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_phase(source_makapix_phase_t phase, const char *code, int64_t deadline_ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.phase = phase;
    snprintf(s_state.code, sizeof(s_state.code), "%s", code != NULL ? code : "");
    s_state.deadline_ms = deadline_ms;
    xSemaphoreGive(s_lock);
    p3a_event_emit_i32(P3A_EVENT_MAKAPIX_CHANGED, 0);
}

static void set_result(const char *what, esp_err_t err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        snprintf(s_state.last_result, sizeof(s_state.last_result), "%s", what);
    } else {
        snprintf(s_state.last_result, sizeof(s_state.last_result), "%s: %s", what, p3a_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
}

void makapix_register_status(source_makapix_status_t *out)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->phase = s_state.phase;
    memcpy(out->code, s_state.code, sizeof(out->code));
    const int64_t left = s_state.deadline_ms - now_ms();
    out->code_seconds_left = s_state.phase == SOURCE_MAKAPIX_SHOWING_CODE && left > 0 ? (uint32_t)(left / 1000) : 0;
    memcpy(out->last_result, s_state.last_result, sizeof(out->last_result));
    xSemaphoreGive(s_lock);
}

// What the task does every second, whatever else it is doing: the link is
// kept and the certificate's dates looked at.
static void upkeep(void)
{
    makapix_link_tick();
    makapix_renew_tick(makapix_link_is_invalid());
}

// Waits up to `ms`, a second at a time with the upkeep in between; true
// when the wait was cut short by a cancel or a new request. A registration
// lasts a quarter of an hour, and a device that is registering again is
// still a player meanwhile.
static bool wait_or_interrupted(uint32_t ms)
{
    for (uint32_t waited = 0; waited < ms; waited += 1000) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms - waited < 1000 ? ms - waited : 1000));
        if (atomic_load(&s_cancel) || atomic_load(&s_want_register)) {
            return true;
        }
        upkeep();
    }
    return false;
}

static void screen(const char *title, const char *text, const char *hint, bool big_title)
{
    p3a_ui_content_t words;
    p3a_ui_content(&words, title, -1, "%s", text);
    words.big_title = big_title;
    snprintf(words.hint, sizeof(words.hint), "%s", hint != NULL ? hint : "");
    p3a_ui_screen_show(P3A_SCREEN_REGISTRATION, &words);
}

static esp_err_t ask_for_code(makapix_provision_t *out)
{
    if (p3a_mem_level() != P3A_MEM_OK) {
        return ESP_ERR_NO_MEM;
    }
    char body[128], answer[1024];
    snprintf(body, sizeof(body), "{\"device_model\":\"p3a\",\"firmware_version\":\"%s\"}",
             esp_app_get_description()->version);
    const http_fetch_header_t headers[] = {{"Content-Type", "application/json"}};
    const http_fetch_request_t req = {.url = "https://" MAKAPIX_HOST "/api/player/provision",
                                      .method = HTTP_FETCH_POST,
                                      .body = body,
                                      .body_len = strlen(body),
                                      .headers = headers,
                                      .header_count = 1,
                                      .user_agent = makapix_user_agent(),
                                      .accept_2xx = true,  // it answers 201
                                      .timeout_ms = 30000,
                                      .max_attempts = 1};
    size_t got = 0;
    esp_err_t err = http_fetch_to_buffer(&req, answer, sizeof(answer), &got, NULL);
    if (err == ESP_OK) {
        err = makapix_parse_provision(answer, out);
    }
    return err;
}

// Asks once for the credentials and, if they are there, saves the
// registration. P3A_ERR_MISSING: the owner has not typed the code yet.
static esp_err_t fetch_and_save(const makapix_provision_t *asked)
{
    char url[160];
    snprintf(url, sizeof(url), "https://" MAKAPIX_HOST "/api/player/%s/credentials", asked->player_key);
    char *answer = psram_malloc(CREDENTIALS_BYTES);
    char *pems = psram_malloc(3 * MAKAPIX_PEM_MAX);
    makapix_credentials_t *got = psram_calloc(1, sizeof(*got));
    esp_err_t err = answer != NULL && pems != NULL && got != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        const http_fetch_request_t req = {
            .url = url, .user_agent = makapix_user_agent(), .timeout_ms = 10000, .max_attempts = 1};
        size_t len = 0;
        err = http_fetch_to_buffer(&req, answer, CREDENTIALS_BYTES, &len, NULL);
    }
    if (err == ESP_OK) {
        err = makapix_parse_credentials(answer, pems, 3 * MAKAPIX_PEM_MAX, got);
    }
    if (answer != NULL) {
        memset(answer, 0, CREDENTIALS_BYTES);  // it held the private key
    }
    if (err == ESP_OK) {
        makapix_identity_t identity = {0};
        memcpy(identity.player_key, asked->player_key, sizeof(identity.player_key));
        // The broker the credentials name, else the one the code came with.
        snprintf(identity.mqtt_host, sizeof(identity.mqtt_host), "%s", got->host[0] != '\0' ? got->host : asked->host);
        identity.mqtt_port = got->port != 0 ? got->port : asked->port;
        // makapix_pems_t holds strings it owns; these are borrowed for the call.
        const makapix_pems_t borrowed = {.ca = (char *)got->ca, .cert = (char *)got->cert, .key = (char *)got->key};
        err = makapix_store_save(&identity, &borrowed, got->token);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "the registration could not be saved: %s", p3a_err_to_name(err));
            err = ESP_ERR_INVALID_STATE;  // not "not yet": it is asked for again and saved again
        }
    }
    if (pems != NULL) {
        memset(pems, 0, 3 * MAKAPIX_PEM_MAX);
    }
    if (got != NULL) {
        memset(got, 0, sizeof(*got));
    }
    free(got);
    free(pems);
    free(answer);
    return err;
}

static void register_run(void)
{
    atomic_store(&s_cancel, false);
    set_phase(SOURCE_MAKAPIX_ASKING, NULL, 0);
    set_result("", ESP_OK);
    screen("Makapix Club", "Asking Makapix Club for a code...", "Long press to hide", false);

    makapix_provision_t *asked = psram_calloc(1, sizeof(*asked));
    esp_err_t err = asked != NULL ? ask_for_code(asked) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no registration code: %s", p3a_err_to_name(err));
        set_result("no code", err);
        screen("Makapix Club",
               err == (esp_err_t)P3A_ERR_RATE_LIMITED ? "Makapix Club is not giving out codes just now. Try again in an hour."
                                                      : "Makapix Club could not be reached. Try again in a moment.",
               "Long press to hide", false);
        set_phase(SOURCE_MAKAPIX_IDLE, NULL, 0);
        (void)wait_or_interrupted(DONE_SCREEN_MS);
        p3a_ui_screen_hide(P3A_SCREEN_REGISTRATION);
        free(asked);
        return;
    }

    // How long the code is good for: the server's word when the device's
    // clock can be trusted to read it, else what the server is known to give.
    uint32_t window_s = CODE_WINDOW_S;
    if (asked->expires_at != 0 && p3a_clock_is_set()) {
        const int64_t left = (int64_t)asked->expires_at - (int64_t)time(NULL);
        if (left >= 60 && left <= CODE_WINDOW_MAX_S) {
            window_s = (uint32_t)left;
        }
    }
    const int64_t deadline = now_ms() + (int64_t)window_s * 1000;
    set_phase(SOURCE_MAKAPIX_SHOWING_CODE, asked->code, deadline);
    const unsigned minutes = (unsigned)((window_s + 59) / 60);  // 14 minutes 58 seconds is "15 minutes"
    ESP_LOGI(TAG, "a registration code is on the screen, good for %u minutes", minutes);
    char text[200];
    snprintf(text, sizeof(text),
             "To add this player to your account, sign in at makapix.club and enter this code.\n\n"
             "It is good for %u minutes.", minutes);
    // The code is the title: it is what must be read from across a desk.
    screen(asked->code, text, "Long press to hide", true);

    const char *ended = NULL;
    bool registered = false;
    while (ended == NULL) {
        if (wait_or_interrupted(POLL_MS)) {
            ended = "cancelled";
        } else if (now_ms() >= deadline) {
            ended = "the code ran out";
        } else if (p3a_mem_level() == P3A_MEM_OK) {
            err = fetch_and_save(asked);
            if (err == ESP_OK) {
                registered = true;
                ended = "registered";
            } else if (err != (esp_err_t)P3A_ERR_MISSING) {
                ESP_LOGW(TAG, "asking for the credentials: %s", p3a_err_to_name(err));  // asked again in three seconds
            }
        }
    }
    memset(asked, 0, sizeof(*asked));
    free(asked);
    ESP_LOGI(TAG, "registration: %s", ended);
    set_result(ended, ESP_OK);
    set_phase(SOURCE_MAKAPIX_IDLE, NULL, 0);
    if (registered) {
        p3a_event_emit_i32(P3A_EVENT_MAKAPIX_CHANGED, 1);
        screen("Registered", "This player is now on your Makapix Club account.", "", false);
        (void)wait_or_interrupted(DONE_SCREEN_MS);
    }
    p3a_ui_screen_hide(P3A_SCREEN_REGISTRATION);
}

static void makapix_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (atomic_exchange(&s_want_register, false)) {
            register_run();
        }
        atomic_store(&s_cancel, false);  // a cancel with nothing to cancel is spent
        upkeep();
    }
}

esp_err_t makapix_task_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const p3a_task_config_t config = P3A_TASK_CONFIG("makapix", makapix_task, NULL, 8192, P3A_TASK_NETWORK);
    return p3a_task_create(&config, &s_task);
}

esp_err_t source_makapix_register(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // A registration already under way is ended first (the loop sees the
    // new request as an interruption), and this one starts from a new code.
    atomic_store(&s_want_register, true);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t source_makapix_register_cancel(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_want_register, false);
    atomic_store(&s_cancel, true);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t source_makapix_unregister(void)
{
    if (!makapix_store_registered()) {
        return P3A_ERR_MISSING;
    }
    const esp_err_t err = makapix_store_clear();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "this device is no longer a Makapix Club player (the site still lists it until it is removed there)");
        set_result("unregistered", ESP_OK);
        p3a_event_emit_i32(P3A_EVENT_MAKAPIX_CHANGED, 1);
    }
    return err;
}
