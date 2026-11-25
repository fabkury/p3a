// SPDX-License-Identifier: Apache-2.0
// Makapix Club over HTTP: where this device stands with it, and registering.
//
//   GET    /api/v2/makapix                 registered; how a registration under way is going; the link
//                                          ("none", "waiting", "connecting", "connected", "invalid");
//                                          when the certificate ends and how its last renewal went
//   POST   /api/v2/makapix/registration    start registering: the device asks for a code and shows it
//   DELETE /api/v2/makapix/registration    end a registration under way; or, with none under way,
//                                          forget the device's registration
//
// A registration is not a job: it lasts until the owner types the code at
// makapix.club, up to a quarter of an hour. The page polls GET.
//
// The player's key is not given out: with it, anyone on the LAN could ask
// Makapix for the player's token (1.x's status showed it).

#include "net_internal.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_net.h"
#include "source_makapix.h"

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
    if (status.phase != SOURCE_MAKAPIX_IDLE) {
        const esp_err_t err = source_makapix_register_cancel();
        err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "ending the registration");
        return;
    }
    const esp_err_t err = source_makapix_unregister();
    if (err == (esp_err_t)P3A_ERR_MISSING) {
        p3a_http_fail(req, 409, "NOT_REGISTERED", "this device is not registered with Makapix Club");
    } else {
        err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "forgetting the registration");
    }
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/makapix", get_makapix},
    {P3A_HTTP_POST, 0, "/api/v2/makapix/registration", post_registration},
    {P3A_HTTP_DELETE, 0, "/api/v2/makapix/registration", delete_registration},
};

void net_makapix_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
