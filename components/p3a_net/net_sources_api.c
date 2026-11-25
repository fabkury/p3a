// SPDX-License-Identifier: Apache-2.0
// What a person can ask of a source, over HTTP.
//
//   GET  /api/v2/sources/museums          the museums this firmware has: id, name, axes, whether one
//                                         needs a key and has it, and why one cannot be played now
//   POST /api/v2/sources/giphy/reset-id   forget the id Giphy gave this device (it shapes what
//                                         Giphy serves); a new one is asked for at the next listing

#include "net_internal.h"
#include "p3a_http.h"
#include <string.h>

#include "source_giphy.h"
#include "source_museum.h"

static void post_giphy_reset_id(p3a_http_request_t *req)
{
    const esp_err_t err = source_giphy_reset_random_id();
    err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "forgetting Giphy's id");
}

static void get_museums(p3a_http_request_t *req)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(data, "museums");
    source_museum_info_t info;
    for (size_t i = 0; source_museum_info(i, &info); i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", info.id);
        cJSON_AddStringToObject(item, "name", info.name);
        cJSON *axes = cJSON_AddArrayToObject(item, "axes");
        for (const char *p = info.axes; *p != '\0';) {
            const size_t n = strcspn(p, " ");
            char axis[32];
            if (n < sizeof(axis)) {
                memcpy(axis, p, n);
                axis[n] = '\0';
                cJSON_AddItemToArray(axes, cJSON_CreateString(axis));
            }
            p += n + (p[n] == ' ' ? 1 : 0);
        }
        cJSON_AddBoolToObject(item, "needs_key", info.needs_key);
        if (info.needs_key) {
            cJSON_AddBoolToObject(item, "key_set", info.key_set);
        }
        if (info.unavailable != NULL) {
            cJSON_AddStringToObject(item, "unavailable", info.unavailable);
        }
        cJSON_AddItemToArray(list, item);
    }
    p3a_http_ok(req, data);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/sources/museums", get_museums},
    {P3A_HTTP_POST, 0, "/api/v2/sources/giphy/reset-id", post_giphy_reset_id},
};

void net_sources_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
