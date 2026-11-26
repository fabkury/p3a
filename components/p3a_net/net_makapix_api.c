// SPDX-License-Identifier: Apache-2.0
// Makapix Club over HTTP: where this device stands with it, and registering.
//
//   GET    /api/v2/makapix                 registered; how a registration under way is going; the link
//                                          ("none", "waiting", "connecting", "connected", "invalid");
//                                          when the certificate ends and how its last renewal went
//   POST   /api/v2/makapix/registration    start registering: the device asks for a code and shows it
//   DELETE /api/v2/makapix/registration    end a registration under way. With none under way it
//                                          does nothing, and says so
//   DELETE /api/v2/makapix                 forget the device's registration
//
// The last two were one call at first, which did the one or the other by
// whether a code was on the screen. A "cancel" that arrived a moment after
// the code ran out then removed the registration: it cost the dev unit its
// own. What cannot be undone has an address to itself.
//   POST   /api/v2/makapix/playset         {"name"}: play one of the server's playsets
//                                          ("followed_artists"). A job; it fails with
//                                          ESP_ERR_NOT_FOUND when the playset is empty
//
// A registration is not a job: it lasts until the owner types the code at
// makapix.club, up to a quarter of an hour. The page polls GET.
//
// The player's key is not given out: with it, anyone on the LAN could ask
// Makapix for the player's token (1.x's status showed it).

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "makapix_command_map.h"
#include "net_internal.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_job.h"
#include "p3a_net.h"
#include "play_scheduler.h"
#include "playset.h"
#include "psram_alloc.h"
#include "source_makapix.h"

static const char *TAG = "makapix_api";

static const char *const s_phases[] = {"idle", "asking", "showing_code"};
static const char *const s_links[] = {"none", "waiting", "connecting", "connected", "invalid"};

static void get_makapix(p3a_http_request_t *req)
{
    source_makapix_status_t status;
    source_makapix_status(&status);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "registered", status.registered);
    cJSON *registration = cJSON_AddObjectToObject(data, "registration");
    cJSON_AddStringToObject(registration, "phase", s_phases[status.phase <= SOURCE_MAKAPIX_SHOWING_CODE ? status.phase : 0]);
    if (status.phase == SOURCE_MAKAPIX_SHOWING_CODE) {
        // The code is on the device's screen for whoever stands before it;
        // the page shows it too, to whoever started this from a phone.
        cJSON_AddStringToObject(registration, "code", status.code);
        cJSON_AddNumberToObject(registration, "seconds_left", status.code_seconds_left);
    }
    if (status.last_result[0] != '\0') {
        cJSON_AddStringToObject(registration, "last_result", status.last_result);
    }
    cJSON_AddStringToObject(data, "link", s_links[status.link <= SOURCE_MAKAPIX_LINK_INVALID ? status.link : 0]);
    if (status.seen_post_id != 0) {
        cJSON_AddNumberToObject(data, "seen_post_id", status.seen_post_id);  // what a view would be reported for
    }
    if (status.registered) {
        if (status.cert_not_after != 0) {
            cJSON_AddNumberToObject(data, "certificate_ends", status.cert_not_after);
        }
        if (status.renewal[0] != '\0') {
            cJSON_AddStringToObject(data, "certificate_renewal", status.renewal);
        }
    }
    p3a_http_no_store(req);  // the seconds run
    p3a_http_ok(req, data);
}

static void post_registration(p3a_http_request_t *req)
{
    if (!p3a_net_is_up()) {
        p3a_http_fail(req, 409, "OFFLINE", "the device has no network to reach Makapix Club over");
        return;
    }
    const esp_err_t err = source_makapix_register();
    err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "starting the registration");
}

static void delete_registration(p3a_http_request_t *req)
{
    source_makapix_status_t status;
    source_makapix_status(&status);
    const bool under_way = status.phase != SOURCE_MAKAPIX_IDLE;
    const esp_err_t err = under_way ? source_makapix_register_cancel() : ESP_OK;
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "ending the registration");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "cancelled", under_way);
    p3a_http_ok(req, data);
}

static void delete_makapix(p3a_http_request_t *req)
{
    const esp_err_t err = source_makapix_unregister();
    if (err == (esp_err_t)P3A_ERR_MISSING) {
        p3a_http_fail(req, 409, "NOT_REGISTERED", "this device is not registered with Makapix Club");
    } else {
        err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "forgetting the registration");
    }
}

// --- one of the server's playsets -----------------------------------------------------
//
// "Play the artists I follow": the server makes that playset for the
// player's owner. It is asked for, read by the reader of the server's
// play_playset command (makapix_command_map), and played. It is not saved:
// it is the server's, and asking again gets it as it is then.

#define SERVER_PLAYSET_ANSWER (48 * 1024)  // 64 channels with long names fit several times over

static esp_err_t server_playset_job(void *arg)
{
    char *name = arg;
    char *answer = psram_malloc(SERVER_PLAYSET_ANSWER);
    makapix_action_t *action = psram_calloc(1, sizeof(*action));
    esp_err_t err = answer != NULL && action != NULL ? source_makapix_get_playset(name, answer, SERVER_PLAYSET_ANSWER)
                                                     : ESP_ERR_NO_MEM;
    cJSON *json = err == ESP_OK ? cJSON_Parse(answer) : NULL;
    if (err == ESP_OK && json == NULL) {
        err = P3A_ERR_CORRUPT;
    }
    if (err == ESP_OK) {
        cJSON *channels = cJSON_GetObjectItemCaseSensitive(json, "channels");
        if (!cJSON_IsArray(channels) || cJSON_GetArraySize(channels) == 0) {
            err = ESP_ERR_NOT_FOUND;  // a playset of nothing: the owner follows nobody yet
        } else {
            // More than a playset holds: the first ones, which the server puts first on purpose.
            while (cJSON_GetArraySize(channels) > PLAYSET_MAX_CHANNELS) {
                cJSON_DeleteItemFromArray(channels, cJSON_GetArraySize(channels) - 1);
            }
            cJSON_DeleteItemFromObjectCaseSensitive(json, "name");  // "name" would save it
            makapix_command_map("play_playset", json, action);
            if (action->kind == MAKAPIX_DO_PLAYSET) {
                err = play_scheduler_activate(action->playset);
            } else {
                ESP_LOGW(TAG, "the server's playset \"%s\" cannot be played: %s", name, action->text);
                err = P3A_ERR_NOT_PLAYABLE;
            }
        }
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "the server's playset \"%s\": %s", name, p3a_err_to_name(err));
    }
    if (action != NULL) {
        makapix_action_free(action);
    }
    cJSON_Delete(json);
    free(action);
    free(answer);
    free(name);
    return err;
}

static void post_playset(p3a_http_request_t *req)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "name");
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0' || strlen(name->valuestring) >= 48) {
        p3a_http_fail(req, 400, "INVALID_NAME", "give \"name\": one of the server's playsets, such as \"followed_artists\"");
        return;
    }
    source_makapix_status_t status;
    source_makapix_status(&status);
    if (!status.registered) {
        p3a_http_fail(req, 409, "NOT_REGISTERED", "this device is not registered with Makapix Club");
        return;
    }
    if (!p3a_net_is_up()) {
        p3a_http_fail(req, 409, "OFFLINE", "the device has no network to reach Makapix Club over");
        return;
    }
    char *copy = psram_malloc(strlen(name->valuestring) + 1);
    if (copy == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "asking for the playset");
        return;
    }
    strcpy(copy, name->valuestring);
    p3a_job_id_t job = P3A_JOB_ID_NONE;
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "makapix playset", server_playset_job, copy, &job);
    if (err != ESP_OK) {
        free(copy);
        p3a_http_fail_err(req, err, "asking for the playset");
        return;
    }
    p3a_http_accepted(req, job);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/makapix", get_makapix},
    {P3A_HTTP_POST, P3A_HTTP_BODY_JSON, "/api/v2/makapix/playset", post_playset},
    {P3A_HTTP_POST, 0, "/api/v2/makapix/registration", post_registration},
    {P3A_HTTP_DELETE, 0, "/api/v2/makapix/registration", delete_registration},
    {P3A_HTTP_DELETE, 0, "/api/v2/makapix", delete_makapix},
};

void net_makapix_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
