// SPDX-License-Identifier: Apache-2.0
// What a person can ask of a source, over HTTP.
//
//   POST /api/v2/sources/giphy/reset-id   forget the id Giphy gave this device (it shapes what
//                                         Giphy serves); a new one is asked for at the next listing

#include "net_internal.h"
#include "p3a_http.h"
#include "source_giphy.h"

static void post_giphy_reset_id(p3a_http_request_t *req)
{
    const esp_err_t err = source_giphy_reset_random_id();
    err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "forgetting Giphy's id");
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_POST, 0, "/api/v2/sources/giphy/reset-id", post_giphy_reset_id},
};

void net_sources_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
