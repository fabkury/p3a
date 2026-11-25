// SPDX-License-Identifier: Apache-2.0
// Decisions and text of the Makapix link. See makapix_link_logic.h. Pure.

#include "makapix_link_logic.h"

#include <stdio.h>
#include <string.h>

#include "p3a_err.h"

// --- reconnecting ---------------------------------------------------------------------

void makapix_retry_reset(makapix_retry_t *r)
{
    r->base_ms = MAKAPIX_RETRY_FIRST_MS;
    r->handshake_failures = 0;
    r->renewal_tried = false;
}

uint32_t makapix_retry_delay(makapix_retry_t *r, uint32_t random)
{
    if (r->base_ms < MAKAPIX_RETRY_FIRST_MS || r->base_ms > MAKAPIX_RETRY_MAX_MS) {
        r->base_ms = MAKAPIX_RETRY_FIRST_MS;
    }
    const uint32_t lo = r->base_ms / 2;
    uint32_t hi = r->base_ms + r->base_ms / 2;
    if (hi > MAKAPIX_RETRY_MAX_MS) {
        hi = MAKAPIX_RETRY_MAX_MS;
    }
    const uint32_t delay = lo + random % (hi - lo + 1);
    r->base_ms = r->base_ms * 2 > MAKAPIX_RETRY_MAX_MS ? MAKAPIX_RETRY_MAX_MS : r->base_ms * 2;
    return delay;
}

makapix_retry_verdict_t makapix_retry_failed(makapix_retry_t *r, bool handshake_failed, bool broker_rejected)
{
    if (!handshake_failed) {
        return MAKAPIX_RETRY_AGAIN;  // the network, a timeout: says nothing about the certificates
    }
    if (r->handshake_failures < 255) {
        r->handshake_failures++;
    }
    const bool due = broker_rejected || r->handshake_failures >= MAKAPIX_HANDSHAKE_FAILURES;
    if (!due) {
        return MAKAPIX_RETRY_AGAIN;
    }
    return r->renewal_tried ? MAKAPIX_RETRY_INVALID : MAKAPIX_RETRY_RENEW;
}

makapix_retry_verdict_t makapix_retry_renewed(makapix_retry_t *r, bool renewed, bool counted)
{
    if (renewed) {
        r->handshake_failures = 0;
        r->renewal_tried = true;  // one per outage: if the new certificates fail too, it is not the certificates
        return MAKAPIX_RETRY_AGAIN;
    }
    if (!counted) {
        return MAKAPIX_RETRY_AGAIN;  // could not even be tried: tried again at the next failure
    }
    r->renewal_tried = true;
    return MAKAPIX_RETRY_INVALID;
}

// --- renewing the certificates ---------------------------------------------------------

makapix_renew_due_t makapix_renew_due(uint32_t now_unix, uint32_t not_after, uint32_t random, bool forced)
{
    if (now_unix < MAKAPIX_CLOCK_PLAUSIBLE) {
        return MAKAPIX_RENEW_NO_CLOCK;
    }
    if (forced) {
        return MAKAPIX_RENEW_DUE;
    }
    if (not_after == 0) {
        return MAKAPIX_RENEW_NOT_DUE;
    }
    if (now_unix >= not_after || not_after - now_unix <= MAKAPIX_RENEW_URGENT_S) {
        return MAKAPIX_RENEW_DUE;
    }
    // From 190 days before the end, later by up to a week. The draw is new
    // at every check, so a player asks, on average, within a few checks of
    // the window opening rather than all on its first day.
    const uint32_t opens = not_after > MAKAPIX_RENEW_WINDOW_S ? not_after - MAKAPIX_RENEW_WINDOW_S : 0;
    const uint32_t jitter = random % MAKAPIX_RENEW_JITTER_S;
    return now_unix >= opens && now_unix - opens >= jitter ? MAKAPIX_RENEW_DUE : MAKAPIX_RENEW_NOT_DUE;
}

// --- views -----------------------------------------------------------------------------

void makapix_views_init(makapix_views_t *v)
{
    memset(v, 0, sizeof(*v));
}

// Brings seen_ms up to now.
static void count(makapix_views_t *v, int64_t now_ms)
{
    if (v->post_id != 0 && !v->covered && now_ms > v->since_ms) {
        v->seen_ms += now_ms - v->since_ms;
    }
    v->since_ms = now_ms;
}

void makapix_views_show(makapix_views_t *v, uint32_t post_id, int64_t now_ms)
{
    if (post_id == v->post_id) {
        return;
    }
    v->post_id = post_id;
    v->seen_ms = 0;
    v->since_ms = now_ms;
    v->next_ms = MAKAPIX_VIEW_FIRST_MS;
}

void makapix_views_cover(makapix_views_t *v, bool covered, int64_t now_ms)
{
    count(v, now_ms);
    v->covered = covered;
}

bool makapix_views_due(makapix_views_t *v, int64_t now_ms)
{
    count(v, now_ms);
    if (v->post_id == 0 || v->covered || v->seen_ms < v->next_ms) {
        return false;
    }
    // The next one is counted from this one: a device that was busy and
    // comes late does not make up for it with a burst.
    v->next_ms = v->seen_ms + MAKAPIX_VIEW_REPEAT_MS;
    return true;
}

// --- what is said ----------------------------------------------------------------------

void makapix_topic(makapix_topic_t which, const char *player_key, char out[MAKAPIX_TOPIC_LEN])
{
    static const char *const tails[MAKAPIX_TOPIC_COUNT] = {"command", "command/ack", "status", "capabilities",
                                                           "state",   "view",        "view/ack"};
    snprintf(out, MAKAPIX_TOPIC_LEN, "makapix/player/%.36s/%s", player_key, which < MAKAPIX_TOPIC_COUNT ? tails[which] : "");
}

void makapix_time_text(uint32_t unix_s, char out[24])
{
    // Civil date from days (the inverse of makapix_time()'s arithmetic).
    const uint32_t days = unix_s / 86400, rest = unix_s % 86400;
    const int64_t z = (int64_t)days + 719468;
    const int64_t era = z / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t day = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = (int64_t)yoe + era * 400 + (month <= 2 ? 1 : 0);
    // Each field is bounded where the compiler can see it: 20 characters and the terminator.
    char text[48];
    snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)(year % 10000), (unsigned)(month % 100),
             (unsigned)(day % 100), (unsigned)(rest / 3600 % 100), (unsigned)(rest % 3600 / 60), (unsigned)(rest % 60));
    memcpy(out, text, 21);
}

static esp_err_t fitted(int n, size_t len)
{
    return n > 0 && (size_t)n < len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

// Text that goes between quotes as it is: no quote, backslash or control character.
static bool plain(const char *text)
{
    if (text == NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        if (*p < 0x20 || *p == '"' || *p == '\\' || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

esp_err_t makapix_msg_status(const char *player_key, bool online, uint32_t current_post_id, const char *version,
                             uint32_t now_unix, char *out, size_t len)
{
    if (!plain(player_key) || !plain(version)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!online) {
        return fitted(snprintf(out, len, "{\"player_key\":\"%s\",\"status\":\"offline\"}", player_key), len);
    }
    char post[16] = "null", when[24];
    if (current_post_id != 0) {
        snprintf(post, sizeof(post), "%u", (unsigned)current_post_id);
    }
    makapix_time_text(now_unix, when);
    return fitted(snprintf(out, len,
                           "{\"player_key\":\"%s\",\"status\":\"online\",\"current_post_id\":%s,"
                           "\"firmware_version\":\"%s\",\"timestamp\":\"%s\"}",
                           player_key, post, version, when),
                  len);
}

esp_err_t makapix_msg_capabilities(const char *version, char *out, size_t len)
{
    if (!plain(version)) {
        return ESP_ERR_INVALID_ARG;
    }
    return fitted(snprintf(out, len,
                           "{\"firmware_version\":\"%s\",\"features\":{\"pause\":{},"
                           "\"brightness\":{\"min\":0,\"max\":100,\"step\":1},\"rotation\":{\"values\":[0,90,180,270]}}}",
                           version),
                  len);
}

esp_err_t makapix_msg_state(bool paused, int brightness, int rotation, char *out, size_t len)
{
    return fitted(snprintf(out, len, "{\"is_paused\":%s,\"brightness\":%d,\"rotation\":%d}", paused ? "true" : "false",
                           brightness, rotation),
                  len);
}

esp_err_t makapix_msg_ack(const char *command_id, const char *status, const char *error, char *out, size_t len)
{
    // Through the JSON builder: the id came from outside and the error is free text.
    cJSON *root = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (root != NULL && cJSON_AddStringToObject(root, "command_id", command_id != NULL ? command_id : "") != NULL &&
        cJSON_AddStringToObject(root, "status", status != NULL ? status : "error") != NULL &&
        (error != NULL ? cJSON_AddStringToObject(root, "error", error) : cJSON_AddNullToObject(root, "error")) != NULL) {
        err = cJSON_PrintPreallocated(root, out, (int)len, false) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t makapix_msg_view(const char *player_key, const makapix_view_t *view, uint32_t now_unix, char *out, size_t len)
{
    cJSON *root = cJSON_CreateObject();
    char when[24];
    makapix_time_text(now_unix, when);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (root != NULL && cJSON_AddNumberToObject(root, "post_id", view->post_id) != NULL &&
        cJSON_AddStringToObject(root, "timestamp", when) != NULL && cJSON_AddStringToObject(root, "timezone", "") != NULL &&
        cJSON_AddStringToObject(root, "intent", view->asked_for ? "artwork" : "channel") != NULL &&
        // 0: the server's order; 1: by date; 2: random. A channel played in its own order is in the server's.
        cJSON_AddNumberToObject(root, "play_order", view->random ? 2 : 0) != NULL &&
        cJSON_AddStringToObject(root, "channel", view->channel != NULL ? view->channel : "unknown") != NULL &&
        cJSON_AddStringToObject(root, "player_key", player_key) != NULL &&
        (view->user_sqid != NULL ? cJSON_AddStringToObject(root, "channel_user_sqid", view->user_sqid)
                                 : cJSON_AddNullToObject(root, "channel_user_sqid")) != NULL &&
        (view->hashtag != NULL ? cJSON_AddStringToObject(root, "channel_hashtag", view->hashtag)
                               : cJSON_AddNullToObject(root, "channel_hashtag")) != NULL &&
        cJSON_AddBoolToObject(root, "request_ack", view->want_ack) != NULL) {
        err = cJSON_PrintPreallocated(root, out, (int)len, false) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t makapix_parse_command(const char *json, size_t len, makapix_command_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    esp_err_t err = P3A_ERR_CORRUPT;
    if (cJSON_IsObject(root)) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "command_type");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "command_id");
        if (cJSON_IsString(type) && type->valuestring[0] != '\0' && strlen(type->valuestring) < sizeof(out->type) &&
            (id == NULL || cJSON_IsNull(id) || (cJSON_IsString(id) && strlen(id->valuestring) < sizeof(out->id)))) {
            strcpy(out->type, type->valuestring);
            if (cJSON_IsString(id)) {
                strcpy(out->id, id->valuestring);
            }
            cJSON *payload = cJSON_DetachItemFromObjectCaseSensitive(root, "payload");
            if (!cJSON_IsObject(payload)) {
                cJSON_Delete(payload);
                payload = cJSON_CreateObject();
            }
            out->payload = payload;
            err = payload != NULL ? ESP_OK : ESP_ERR_NO_MEM;
        }
    }
    cJSON_Delete(root);
    return err;
}
