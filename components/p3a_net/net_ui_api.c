// SPDX-License-Identifier: Apache-2.0
// What is on the device's screen, over HTTP.
//
//   GET /api/v2/ui    the screen on top, the notice if one is seen, and its words
//
// The web page uses it to say what the device is showing (a notice, the
// setup screen); tests use it to check the screen model on the device.

#include "net_internal.h"
#include "p3a_http.h"
#include "p3a_ui.h"

static void get_ui(p3a_http_request_t *req)
{
    p3a_ui_state_t state;
    p3a_ui_get_state(&state);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "screen", p3a_ui_screen_name(state.screen));
    if (state.banner || state.screen == P3A_SCREEN_NOTICE) {
        cJSON_AddStringToObject(data, "notice", p3a_ui_notice_name(state.notice));
    }
    cJSON_AddBoolToObject(data, "banner", state.banner);
    cJSON_AddBoolToObject(data, "covers_artwork", state.covers_artwork);
    if (state.has_content) {
        cJSON_AddStringToObject(data, "title", state.content.title);
        cJSON_AddStringToObject(data, "text", state.content.text);
        if (state.content.progress >= 0) {
            cJSON_AddNumberToObject(data, "progress", state.content.progress);
        }
    }
    cJSON_AddNumberToObject(data, "revision", state.revision);
    p3a_http_ok(req, data);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/ui", get_ui},
};

void net_ui_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
