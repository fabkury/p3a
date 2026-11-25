// SPDX-License-Identifier: Apache-2.0
// Makapix Club over HTTP: where this device stands with it.
//
//   GET /api/v2/makapix    {"registered": bool}
//
// The player's key is not given out: with it, anyone on the LAN could ask
// Makapix for the player's token (1.x's status showed it).

#include "net_internal.h"
#include "p3a_http.h"
#include "source_makapix.h"

static void get_makapix(p3a_http_request_t *req)
{
    source_makapix_status_t status;
    source_makapix_status(&status);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "registered", status.registered);
    p3a_http_ok(req, data);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/makapix", get_makapix},
};

void net_makapix_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
