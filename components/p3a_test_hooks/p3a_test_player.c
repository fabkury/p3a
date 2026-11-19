// SPDX-License-Identifier: Apache-2.0
// Test hook for the animation player: play a file, and hear what the player
// reported about it.
//
//   POST /api/v2/test/play[?smooth=1][&wait=0]
//        body: the file. It is saved under the card's temporary folder and
//        played from there. Unless wait=0, answers when the player has
//        reported the request shown or failed (or after 15 s).
//   POST /api/v2/test/play?path=<absolute path>[&smooth=1][&wait=0]
//        no body: plays a file that is, or is not, already on the card.
//   POST /api/v2/test/play/stop           nothing playing; the panel goes black
//   GET  /api/v2/test/play                the player's status and its last reports
//   POST /api/v2/test/play/blank?on=1|0   black frames, as pause will ask for
//
// The hook is the player's listener while the test build has nothing else
// that listens (the loader pipeline arrives in M2.4).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animation_player.h"
#include "display_renderer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fs_atomic.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"
#include "sd_path.h"

#define MAX_FILE_BYTES (6 * 1024 * 1024)
#define REPORTS        16

typedef struct {
    uint32_t token;
    char what[8];  // "ready", "shown", "failed"
    esp_err_t err;
} report_t;

static SemaphoreHandle_t s_lock;
static report_t s_reports[REPORTS];
static uint32_t s_report_count;  // reports so far; report n is in slot n % REPORTS
static uint32_t s_next_token = 1;
static uint32_t s_uploads;

static void note(uint32_t token, const char *what, esp_err_t err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    report_t *r = &s_reports[s_report_count % REPORTS];
    r->token = token;
    strlcpy(r->what, what, sizeof(r->what));
    r->err = err;
    s_report_count++;
    xSemaphoreGive(s_lock);
}

static void on_ready(uint32_t token, void *ctx)
{
    (void)ctx;
    note(token, "ready", ESP_OK);
}

static void on_shown(uint32_t token, void *ctx)
{
    (void)ctx;
    note(token, "shown", ESP_OK);
}

static void on_failed(uint32_t token, esp_err_t err, void *ctx)
{
    (void)ctx;
    note(token, "failed", err);
}

// The request's final report, or NULL-what if it has none yet.
static report_t outcome_of(uint32_t token)
{
    report_t found = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < REPORTS && i < s_report_count; i++) {
        const report_t *r = &s_reports[i];
        if (r->token == token && strcmp(r->what, "ready") != 0) {
            found = *r;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

static cJSON *status_json(void)
{
    animation_player_status_t st;
    animation_player_get_status(&st);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "token", st.token);
    cJSON_AddNumberToObject(data, "width", st.width);
    cJSON_AddNumberToObject(data, "height", st.height);
    cJSON_AddNumberToObject(data, "frame_count", st.frame_count);
    cJSON_AddNumberToObject(data, "frames_shown", st.frames_shown);
    cJSON_AddNumberToObject(data, "draws", st.draws);
    cJSON_AddNumberToObject(data, "loops", st.loops);
    cJSON_AddBoolToObject(data, "decode_failed", st.decode_failed);
    cJSON_AddBoolToObject(data, "loading", st.loading);
    cJSON_AddNumberToObject(data, "rotation", display_renderer_get_rotation());
    cJSON_AddNumberToObject(data, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(data, "internal_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON *reports = cJSON_AddArrayToObject(data, "reports");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t first = s_report_count > REPORTS ? s_report_count - REPORTS : 0;
    for (uint32_t n = first; n < s_report_count; n++) {
        const report_t *r = &s_reports[n % REPORTS];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "token", r->token);
        cJSON_AddStringToObject(item, "what", r->what);
        if (r->err != ESP_OK) {
            cJSON_AddStringToObject(item, "error", p3a_err_to_name(r->err));
        }
        cJSON_AddItemToArray(reports, item);
    }
    xSemaphoreGive(s_lock);
    return data;
}

// Answers with the status, the request's token and how it ended.
static void reply_outcome(p3a_http_request_t *req, uint32_t token, bool wait)
{
    report_t end = {0};
    for (int waited_ms = 0; wait && waited_ms < 15000; waited_ms += 10) {
        end = outcome_of(token);
        if (end.what[0] != '\0') {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (end.what[0] != '\0' && strcmp(end.what, "shown") == 0) {
        vTaskDelay(pdMS_TO_TICKS(60));  // the frame just drawn reaches the glass a few refreshes later
    }
    cJSON *data = status_json();
    cJSON_AddNumberToObject(data, "request", token);
    cJSON_AddStringToObject(data, "outcome", end.what[0] ? end.what : "pending");
    if (end.err != ESP_OK) {
        cJSON_AddStringToObject(data, "error", p3a_err_to_name(end.err));
    }
    p3a_http_ok(req, data);
}

static void post_play(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    char text[8];
    animation_player_request_t request = {0};
    request.smooth = p3a_http_query(req, "smooth", text, sizeof(text)) && text[0] == '1';
    const bool wait = !(p3a_http_query(req, "wait", text, sizeof(text)) && text[0] == '0');

    if (!p3a_http_query(req, "path", request.path, sizeof(request.path))) {
        const size_t len = raw->content_len;
        if (len == 0 || len > MAX_FILE_BYTES) {
            p3a_http_fail(req, len == 0 ? 400 : 413, len == 0 ? "INVALID" : "TOO_LARGE",
                          "send the file as the body, or name one with path=");
            return;
        }
        uint8_t *file = psram_malloc(len);
        if (file == NULL) {
            p3a_http_fail_err(req, ESP_ERR_NO_MEM, "holding the file");
            return;
        }
        for (size_t got = 0; got < len;) {
            int n = httpd_req_recv(raw, (char *)file + got, len - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (n <= 0) {
                free(file);
                p3a_http_fail(req, 400, "READ_BODY", "the file did not arrive");
                return;
            }
            got += (size_t)n;
        }
        // Two names in turn: the file being replaced is never the one a
        // request still in flight is reading.
        char dir[160];
        esp_err_t err = sd_path_get_temporary(dir, sizeof(dir));
        if (err == ESP_OK) {
            snprintf(request.path, sizeof(request.path), "%s/test-play-%u.bin", dir, (unsigned)(s_uploads++ & 1));
            err = sd_path_ensure_parent_dirs(request.path);
        }
        if (err == ESP_OK) {
            err = fs_atomic_write(request.path, file, len, NULL);
        }
        free(file);
        if (err != ESP_OK) {
            p3a_http_fail_err(req, err, "saving the file to play");
            return;
        }
    }
    request.token = s_next_token++;
    esp_err_t err = animation_player_play(&request);
    if (err == ESP_OK) {
        err = display_renderer_select_source(DISPLAY_SOURCE_ARTWORK);
    }
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "asking the player");
        return;
    }
    reply_outcome(req, request.token, wait);
}

static void post_stop(p3a_http_request_t *req)
{
    const uint32_t token = s_next_token++;
    esp_err_t err = animation_player_stop(token);
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "asking the player");
        return;
    }
    display_renderer_set_blank(false);
    reply_outcome(req, token, true);
}

static void get_play(p3a_http_request_t *req)
{
    p3a_http_ok(req, status_json());
}

static void post_blank(p3a_http_request_t *req)
{
    char text[4];
    display_renderer_set_blank(p3a_http_query(req, "on", text, sizeof(text)) && text[0] == '1');
    vTaskDelay(pdMS_TO_TICKS(250));  // a few frames, for the change to be on the panel
    p3a_http_ok(req, NULL);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_POST, P3A_HTTP_RAW, "/api/v2/test/play", post_play},
    {P3A_HTTP_GET, 0, "/api/v2/test/play", get_play},
    {P3A_HTTP_POST, 0, "/api/v2/test/play/stop", post_stop},
    {P3A_HTTP_POST, 0, "/api/v2/test/play/blank", post_blank},
};

void p3a_test_player_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE("p3a_test", "the player hook did not start");
        return;
    }
    static const animation_player_listener_t listener = {.ready = on_ready, .shown = on_shown, .failed = on_failed};
    animation_player_set_listener(&listener, NULL);
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
