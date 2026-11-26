// SPDX-License-Identifier: Apache-2.0
// The firmware update over HTTP.
//
//   GET  /api/v2/update            what runs, what is known of releases, what is going on
//   POST /api/v2/update/check      look for releases now (a job)
//   POST /api/v2/update/install    install the newer release that was found (a job; the device
//                                  restarts when it ends well)
//   POST /api/v2/update/rollback   go back to the firmware in the other slot (a job; restarts)

#include <stdlib.h>

#include "cJSON.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_update.h"
#include "psram_alloc.h"
#include "update_internal.h"

static const char *const s_states[] = {"idle", "checking", "installing", "restarting"};

static void get_update(p3a_http_request_t *req)
{
    p3a_update_status_t *st = psram_malloc(sizeof(*st));
    if (st == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the update's state");
        return;
    }
    p3a_update_status(st);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "running", st->running);
    cJSON_AddStringToObject(data, "state", s_states[st->state]);
    if (st->state == P3A_UPDATE_INSTALLING) {
        cJSON_AddNumberToObject(data, "progress", st->progress);
    }
    cJSON_AddBoolToObject(data, "on_trial", st->on_trial);
    if (st->other[0] != '\0') {
        cJSON_AddStringToObject(data, "other", st->other);  // can be gone back to
    }
    if (st->undone[0] != '\0') {
        cJSON_AddStringToObject(data, "undone", st->undone);  // was installed, did not prove itself
    }
    cJSON_AddBoolToObject(data, "checked", st->checked);
    if (st->checked) {
        cJSON_AddNumberToObject(data, "checked_ago_s", st->checked_ago_s);
        if (st->check_error != ESP_OK) {
            cJSON_AddStringToObject(data, "check_error", p3a_err_to_name(st->check_error));
        }
    }
    cJSON_AddBoolToObject(data, "available", st->available);
    if (st->has_latest) {
        cJSON *latest = cJSON_AddObjectToObject(data, "latest");
        cJSON_AddStringToObject(latest, "version", st->latest);
        cJSON_AddBoolToObject(latest, "prerelease", st->latest_prerelease);
        cJSON_AddNumberToObject(latest, "bytes", st->latest_bytes);
        cJSON_AddStringToObject(latest, "published", st->latest_published);
        cJSON_AddStringToObject(latest, "notes", st->latest_notes);
    }
    if (st->install_error != ESP_OK) {
        cJSON *error = cJSON_AddObjectToObject(data, "install_error");
        cJSON_AddStringToObject(error, "code", p3a_err_to_name(st->install_error));
        cJSON_AddStringToObject(error, "message", st->install_error_text);
    }
    if (st->blocked != NULL) {
        cJSON_AddStringToObject(data, "blocked", st->blocked);
    }
    free(st);
    p3a_http_ok(req, data);
}

static void answer(p3a_http_request_t *req, esp_err_t err, p3a_job_id_t job, const char *what)
{
    if (err == ESP_OK) {
        p3a_http_accepted(req, job);
    } else if (err == ESP_ERR_INVALID_STATE) {
        p3a_http_fail(req, 409, "BUSY", "the update is busy; see GET /api/v2/update");
    } else {
        p3a_http_fail_err(req, err, what);
    }
}

static void post_check(p3a_http_request_t *req)
{
    p3a_job_id_t job = 0;
    answer(req, p3a_update_check(&job), job, "looking for releases");
}

static void post_install(p3a_http_request_t *req)
{
    p3a_job_id_t job = 0;
    const esp_err_t err = p3a_update_install(&job);
    if (err == ESP_ERR_NOT_FOUND) {
        p3a_http_fail(req, 409, "NOTHING_TO_INSTALL", "no newer release is known; look for one first (POST /api/v2/update/check)");
    } else if (err == P3A_ERR_SD_FAILED) {
        p3a_http_fail(req, 409, "CARD_IN_USE", "a computer has the SD card over USB; unplug it first");
    } else {
        answer(req, err, job, "starting the install");
    }
}

static void post_rollback(p3a_http_request_t *req)
{
    p3a_job_id_t job = 0;
    const esp_err_t err = p3a_update_roll_back(&job);
    if (err == ESP_ERR_NOT_FOUND) {
        p3a_http_fail(req, 409, "NOTHING_TO_GO_BACK_TO", "the other slot holds no firmware");
    } else {
        answer(req, err, job, "going back");
    }
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/update", get_update},
    {P3A_HTTP_POST, 0, "/api/v2/update/check", post_check},
    {P3A_HTTP_POST, 0, "/api/v2/update/install", post_install},
    {P3A_HTTP_POST, 0, "/api/v2/update/rollback", post_rollback},
};

esp_err_t update_api_register(void)
{
    return p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
