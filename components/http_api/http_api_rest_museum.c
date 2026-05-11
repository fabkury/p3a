// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_api_rest_museum.c
 * @brief REST handlers for art-institution (museum) rate-limit sharing
 *
 * Two endpoints, both small. The browse modal and landing-page badge use
 * them so the device's per-museum cooldown table stays in sync with what
 * the browser observed directly from museum APIs (AIC's 60-req/min cap is
 * per-IP, so browser- and device-issued requests share the budget).
 *
 *   GET  /api/museum/rate-limits
 *     -> { "artic": { "remaining_sec": N }, ... }
 *
 *   POST /api/museum/rate-limits/report-429
 *     <- { "museum": "artic", "retry_after_sec": 38 }
 *     -> { "ok": true }   (or 400 on malformed input)
 *
 * See docs/art-institutions/finalized-design.md §6 and §11.1.
 */

#include "http_api_internal.h"
#include "art_institution.h"

esp_err_t h_get_museum_rate_limits(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"NO_MEM\"}");
        return ESP_OK;
    }

    // One entry per registered museum; ordering follows the dispatch table.
    for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
        const art_institution_museum_t *m = &ART_INSTITUTION_MUSEUMS[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;
        cJSON_AddNumberToObject(obj, "remaining_sec",
                                (double)art_institution_rate_limit_remaining(m->id));
        cJSON_AddItemToObject(root, m->id, obj);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"SERIALIZE\"}");
        return ESP_OK;
    }
    send_json(req, 200, body);
    free(body);
    return ESP_OK;
}

esp_err_t h_post_museum_report_429(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"CONTENT_TYPE\"}");
        return ESP_OK;
    }

    int err_status = 0;
    size_t body_len = 0;
    char *body = recv_body_json(req, &body_len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 400, "{\"ok\":false,\"error\":\"BAD_BODY\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"BAD_JSON\"}");
        return ESP_OK;
    }

    const cJSON *museum = cJSON_GetObjectItemCaseSensitive(root, "museum");
    const cJSON *retry  = cJSON_GetObjectItemCaseSensitive(root, "retry_after_sec");
    if (!cJSON_IsString(museum) || !museum->valuestring || !museum->valuestring[0]) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"error\":\"MUSEUM_REQUIRED\"}");
        return ESP_OK;
    }

    // retry_after_sec is optional — 0 triggers the museum default (60 s).
    uint32_t retry_sec = 0;
    if (cJSON_IsNumber(retry)) {
        double v = cJSON_GetNumberValue(retry);
        if (v > 0 && v <= 3600) retry_sec = (uint32_t)v;
    }

    // art_institution_set_rate_limited silently ignores unknown museums,
    // so the response is the same whether the report landed on a known
    // museum or not (don't leak the museum list to opportunistic POSTs).
    art_institution_set_rate_limited(museum->valuestring, retry_sec);
    cJSON_Delete(root);

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}
