// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_api_rest_pinned.c
 * @brief Pinned-artworks REST endpoints
 *
 * Routes (all under /api/pin-lists):
 *   GET    /api/pin-lists                                  → enumerate
 *   POST   /api/pin-lists                                  → create (body: {name})
 *   GET    /api/pin-lists/{slug}                           → list info
 *   POST   /api/pin-lists/{slug}/rename                    → rename (body: {name})
 *   POST   /api/pin-lists/{slug}/active                    → set active
 *   DELETE /api/pin-lists/{slug}                           → delete
 *   GET    /api/pin-lists/{slug}/items?offset=&limit=      → paginated browse
 *   POST   /api/pin-lists/{slug}/items                     → pin (raw, full body)
 *   GET    /api/pin-lists/{slug}/items/{source}/{source_id}        → rich metadata
 *   DELETE /api/pin-lists/{slug}/items/{source}/{source_id}        → unpin
 *   GET    /api/pin-lists/{slug}/items/{source}/{source_id}/local  → artwork bytes
 *
 * `/action/pin` and `/action/unpin` are deferred to phase 3 (gesture dispatcher).
 */

#include "http_api_internal.h"
#include "pin_lists.h"
#include "play_scheduler.h"
#include "p3a_state.h"
#include "giphy.h"
#include "art_institution.h"
#include "makapix_channel_utils.h"
#include "esp_log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PIN_PREFIX        "/api/pin-lists"
#define PIN_PREFIX_LEN    14

#define DEFAULT_PAGE_SIZE 20
#define MAX_PAGE_SIZE     100

/* ------------------------------------------------------------------------- */
/*  Source enum <-> string                                                   */
/* ------------------------------------------------------------------------- */

static const char *source_to_string(pinned_source_t s)
{
    switch (s) {
        case PINNED_SOURCE_MAKAPIX:     return "makapix";
        case PINNED_SOURCE_GIPHY:       return "giphy";
        case PINNED_SOURCE_INSTITUTION: return "museum";
        default:                        return "unknown";
    }
}

static pinned_source_t source_from_string(const char *s)
{
    if (!s) return PINNED_SOURCE_NONE;
    if (strcmp(s, "makapix") == 0) return PINNED_SOURCE_MAKAPIX;
    if (strcmp(s, "giphy") == 0)   return PINNED_SOURCE_GIPHY;
    if (strcmp(s, "museum") == 0)  return PINNED_SOURCE_INSTITUTION;
    return PINNED_SOURCE_NONE;
}

static const char *extension_to_string(uint8_t ext)
{
    switch (ext) {
        case 0: return "webp";
        case 1: return "gif";
        case 2: return "png";
        case 3: return "jpg";
        default: return "webp";
    }
}

static int extension_from_string(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "webp") == 0) return 0;
    if (strcmp(s, "gif") == 0)  return 1;
    if (strcmp(s, "png") == 0)  return 2;
    if (strcmp(s, "jpg") == 0 || strcmp(s, "jpeg") == 0) return 3;
    return -1;
}

/* ------------------------------------------------------------------------- */
/*  Path parsing                                                             */
/* ------------------------------------------------------------------------- */

/**
 * Parse a URI of the form `/api/pin-lists[/{slug}[/...]]` and split into
 * components.
 *
 *   in:  /api/pin-lists/abc12345/items/giphy/foo?offset=0
 *   out: slug="abc12345", remainder="items/giphy/foo"
 *
 * The query string (after '?') is stripped. URL-encoded segments are NOT
 * decoded here — handlers that need decoded values decode their own slice.
 *
 * @return ESP_OK on success even when no slug is present (out_slug[0]=0,
 *         remainder=""). ESP_ERR_INVALID_ARG if the prefix doesn't match.
 */
static esp_err_t split_uri(const char *uri,
                           char out_slug[PIN_LIST_SLUG_LEN],
                           char *out_remainder, size_t out_remainder_len)
{
    out_slug[0] = '\0';
    if (out_remainder_len > 0) out_remainder[0] = '\0';

    if (strncmp(uri, PIN_PREFIX, PIN_PREFIX_LEN) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = uri + PIN_PREFIX_LEN;
    /* "/api/pin-lists" with no trailing slash */
    if (*p == '\0') return ESP_OK;
    /* "/api/pin-lists?query" */
    if (*p == '?') return ESP_OK;
    if (*p != '/') return ESP_ERR_INVALID_ARG;
    p++;  /* skip '/' */

    /* End of path or query → no slug */
    if (*p == '\0' || *p == '?') return ESP_OK;

    /* Extract slug up to '/', '?', or end. */
    const char *slug_start = p;
    while (*p && *p != '/' && *p != '?') p++;
    size_t slug_len = (size_t)(p - slug_start);
    if (slug_len >= PIN_LIST_SLUG_LEN) return ESP_ERR_INVALID_ARG;
    memcpy(out_slug, slug_start, slug_len);
    out_slug[slug_len] = '\0';

    /* Remainder: everything between the slash after slug and the '?' (or end). */
    if (*p == '/') {
        p++;
        const char *qmark = strchr(p, '?');
        size_t rem_len = qmark ? (size_t)(qmark - p) : strlen(p);
        if (rem_len >= out_remainder_len) return ESP_ERR_INVALID_SIZE;
        memcpy(out_remainder, p, rem_len);
        out_remainder[rem_len] = '\0';
    }
    return ESP_OK;
}

static bool parse_query_size(const char *uri, const char *key, size_t *out_val)
{
    const char *qmark = strchr(uri, '?');
    if (!qmark) return false;
    const char *q = qmark + 1;
    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            char buf[16] = {0};
            const char *v = q + key_len + 1;
            const char *end = v;
            while (*end && *end != '&') end++;
            size_t vlen = (size_t)(end - v);
            if (vlen >= sizeof(buf)) vlen = sizeof(buf) - 1;
            memcpy(buf, v, vlen);
            char *endp;
            long n = strtol(buf, &endp, 10);
            if (endp == buf || n < 0) return false;
            *out_val = (size_t)n;
            return true;
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/*  JSON helpers                                                             */
/* ------------------------------------------------------------------------- */

static cJSON *json_from_list_info(const pin_list_info_t *info)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "slug", info->slug);
    cJSON_AddStringToObject(o, "name", info->name);
    cJSON_AddNumberToObject(o, "created_at", info->created_at);
    cJSON_AddNumberToObject(o, "count", info->count);
    cJSON_AddBoolToObject(o, "is_active", info->is_active);
    return o;
}

/* Map a museum enum ordinal to its public string id. The art_institution
 * enum is documented as append-only with stable ordinals; museums shipped
 * since v0.1 are listed below in the same order. */
static const char *museum_id_to_str(uint16_t id)
{
    static const char *names[] = { "artic", "rijks", "vam", "wellcome", "smk", "loc" };
    if (id < (sizeof(names) / sizeof(names[0]))) return names[id];
    return NULL;
}

/* Build the source-CDN URL for a pinned entry into `out`. Empty string on
 * failure (caller falls back to /local on the web UI side).
 *
 * - Makapix: https://{makapix_host}/api/vault/{a}/{b}/{c}/{uuid}.{ext}
 *   (SHA256 prefix derived from the UUID string).
 * - Giphy:   https://i.giphy.com/media/{giphy_id}/giphy.webp
 *   (Giphy CDN always serves WebP for trending/search results).
 * - Museum:  art_institution_build_iiif_url(museum_str, entry-shim).
 *   Note: LoC iiif_keys contain colons that we replace with underscores
 *   when vaulting to FAT, so URL reconstruction will fail for LoC pins —
 *   /local falls in. Other museums round-trip cleanly. */
static void build_source_url(const pinned_order_entry_t *e, char *out, size_t out_len)
{
    if (!e || !out || out_len == 0) return;
    out[0] = '\0';

    static const char *ext_str[] = { ".webp", ".gif", ".png", ".jpg" };
    int ext_idx = (e->extension <= 3) ? e->extension : 0;

    switch ((pinned_source_t)e->source) {
        case PINNED_SOURCE_MAKAPIX: {
            const uint8_t *u = e->makapix.storage_key_uuid;
            char uuid[40];
            snprintf(uuid, sizeof(uuid),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            uint8_t sha[32];
            if (storage_key_sha256(uuid, sha) != ESP_OK) return;
            snprintf(out, out_len,
                     "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                     CONFIG_MAKAPIX_CLUB_HOST,
                     (unsigned)sha[0], (unsigned)sha[1], (unsigned)sha[2],
                     uuid, ext_str[ext_idx]);
            break;
        }
        case PINNED_SOURCE_GIPHY: {
            char gid[PINNED_GIPHY_ID_MAX + 1];
            strlcpy(gid, e->giphy.giphy_id, sizeof(gid));
            snprintf(out, out_len, "https://i.giphy.com/media/%s/giphy.webp", gid);
            break;
        }
        case PINNED_SOURCE_INSTITUTION: {
            const char *museum_str = museum_id_to_str(e->museum.museum_id);
            if (!museum_str) return;
            institution_channel_entry_t shim = {0};
            shim.extension = e->extension;
            strlcpy(shim.iiif_key, e->museum.iiif_key, sizeof(shim.iiif_key));
            (void)art_institution_build_iiif_url(museum_str, &shim, 720, out, out_len);
            break;
        }
        default:
            break;
    }
}

static cJSON *json_from_order_entry(const pinned_order_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "post_id", e->post_id);
    cJSON_AddNumberToObject(o, "pinned_at", e->pinned_at);
    cJSON_AddStringToObject(o, "source", source_to_string((pinned_source_t)e->source));
    cJSON_AddStringToObject(o, "extension", extension_to_string(e->extension));
    /* Reconstructed source-CDN URL (empty when reconstruction isn't possible,
       e.g. LoC iiif_keys whose colons we sanitized for FAT). The web UI uses
       this as its primary thumbnail src and falls back to /local on error. */
    char source_url[300];
    build_source_url(e, source_url, sizeof(source_url));
    cJSON_AddStringToObject(o, "source_url", source_url);
    switch ((pinned_source_t)e->source) {
        case PINNED_SOURCE_MAKAPIX: {
            const uint8_t *u = e->makapix.storage_key_uuid;
            char uuid[40];
            snprintf(uuid, sizeof(uuid),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            cJSON_AddStringToObject(o, "uuid", uuid);
            break;
        }
        case PINNED_SOURCE_GIPHY: {
            char giphy_id[PINNED_GIPHY_ID_MAX + 1];
            strlcpy(giphy_id, e->giphy.giphy_id, sizeof(giphy_id));
            cJSON_AddStringToObject(o, "giphy_id", giphy_id);
            break;
        }
        case PINNED_SOURCE_INSTITUTION: {
            char iiif[PINNED_IIIF_KEY_MAX + 1];
            strlcpy(iiif, e->museum.iiif_key, sizeof(iiif));
            cJSON_AddNumberToObject(o, "museum_id", e->museum.museum_id);
            cJSON_AddStringToObject(o, "iiif_key", iiif);
            break;
        }
        default:
            break;
    }
    return o;
}

static cJSON *json_from_entry_file(const pinned_entry_file_t *e)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "source", source_to_string((pinned_source_t)e->source));
    cJSON_AddStringToObject(o, "source_id", e->source_id);
    cJSON_AddNumberToObject(o, "post_id", e->post_id);
    cJSON_AddNumberToObject(o, "original_post_id", e->original_post_id);
    cJSON_AddNumberToObject(o, "pinned_at", e->pinned_at);
    cJSON_AddNumberToObject(o, "original_created_at", e->original_created_at);
    cJSON_AddStringToObject(o, "extension", extension_to_string(e->extension));
    cJSON_AddNumberToObject(o, "museum_id", e->museum_id);
    cJSON_AddStringToObject(o, "title", e->title);
    cJSON_AddStringToObject(o, "creator", e->creator);
    return o;
}

/* ------------------------------------------------------------------------- */
/*  err mapping                                                              */
/* ------------------------------------------------------------------------- */

static void send_pin_err(httpd_req_t *req, esp_err_t err, const char *context)
{
    int status;
    const char *code;
    switch (err) {
        case ESP_ERR_INVALID_ARG:    status = 400; code = "INVALID_ARG"; break;
        case ESP_ERR_NOT_FOUND:      status = 404; code = "NOT_FOUND"; break;
        case ESP_ERR_INVALID_STATE:  status = 409; code = "CONFLICT"; break;
        case ESP_ERR_NO_MEM:         status = 413; code = "LIST_FULL"; break;
        case ESP_ERR_NOT_SUPPORTED:  status = 503; code = "VERSION_MISMATCH"; break;
        case ESP_ERR_INVALID_CRC:    status = 503; code = "CORRUPT"; break;
        default:                     status = 500; code = "INTERNAL"; break;
    }
    char body[160];
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"%s\",\"code\":\"%s\"}",
             context ? context : esp_err_to_name(err), code);
    send_json(req, status, body);
}

/* ------------------------------------------------------------------------- */
/*  GET handlers                                                             */
/* ------------------------------------------------------------------------- */

static esp_err_t h_get_collection(httpd_req_t *req)
{
    pin_list_info_t infos[PIN_LISTS_MAX_LISTS];
    size_t n = 0;
    esp_err_t err = pin_lists_enumerate(infos, PIN_LISTS_MAX_LISTS, &n);
    if (err != ESP_OK) { send_pin_err(req, err, "enumerate"); return ESP_OK; }

    char active[PIN_LIST_SLUG_LEN] = {0};
    pin_lists_get_active(active);

    cJSON *root = cJSON_CreateObject();
    cJSON *lists = cJSON_AddArrayToObject(root, "lists");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = json_from_list_info(&infos[i]);
        if (o) cJSON_AddItemToArray(lists, o);
    }
    cJSON_AddStringToObject(root, "active", active);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) { send_json(req, 500, "{\"ok\":false,\"code\":\"OOM\"}"); return ESP_OK; }
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

static esp_err_t h_get_list_info(httpd_req_t *req, const char *slug)
{
    pin_list_info_t info;
    esp_err_t err = pin_lists_get_info(slug, &info);
    if (err != ESP_OK) { send_pin_err(req, err, "get_info"); return ESP_OK; }
    cJSON *o = json_from_list_info(&info);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!out) { send_json(req, 500, "{\"ok\":false,\"code\":\"OOM\"}"); return ESP_OK; }
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

static esp_err_t h_get_items(httpd_req_t *req, const char *slug)
{
    size_t offset = 0;
    size_t limit = DEFAULT_PAGE_SIZE;
    parse_query_size(req->uri, "offset", &offset);
    parse_query_size(req->uri, "limit", &limit);
    if (limit == 0) limit = DEFAULT_PAGE_SIZE;
    if (limit > MAX_PAGE_SIZE) limit = MAX_PAGE_SIZE;

    pinned_order_entry_t *page = calloc(limit, sizeof(*page));
    if (!page) { send_json(req, 500, "{\"ok\":false,\"code\":\"OOM\"}"); return ESP_OK; }

    size_t got = 0;
    size_t total = 0;
    esp_err_t err = pin_list_list(slug, offset, limit, page, &got, &total);
    if (err != ESP_OK) {
        free(page);
        send_pin_err(req, err, "list_items");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "slug", slug);
    cJSON_AddNumberToObject(root, "offset", offset);
    cJSON_AddNumberToObject(root, "limit", limit);
    cJSON_AddNumberToObject(root, "total", total);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < got; i++) {
        cJSON *o = json_from_order_entry(&page[i]);
        if (o) cJSON_AddItemToArray(items, o);
    }
    free(page);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) { send_json(req, 500, "{\"ok\":false,\"code\":\"OOM\"}"); return ESP_OK; }
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

static esp_err_t h_get_item(httpd_req_t *req, const char *slug,
                            pinned_source_t src, const char *source_id)
{
    pinned_entry_file_t entry;
    esp_err_t err = pin_list_get_entry(slug, src, source_id, &entry);
    if (err != ESP_OK) { send_pin_err(req, err, "get_entry"); return ESP_OK; }

    cJSON *o = json_from_entry_file(&entry);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!out) { send_json(req, 500, "{\"ok\":false,\"code\":\"OOM\"}"); return ESP_OK; }
    send_json(req, 200, out);
    free(out);
    return ESP_OK;
}

static esp_err_t h_get_item_local(httpd_req_t *req, const char *slug,
                                  pinned_source_t src, const char *source_id)
{
    /* Resolve the order-entry record so we can build the on-disk path. */
    pinned_entry_file_t entry;
    esp_err_t err = pin_list_get_entry(slug, src, source_id, &entry);
    if (err != ESP_OK) { send_pin_err(req, err, "get_entry"); return ESP_OK; }

    /* Reconstruct a minimal order entry just for path building. */
    pinned_order_entry_t shim = {0};
    shim.source = entry.source;
    shim.extension = entry.extension;
    switch ((pinned_source_t)entry.source) {
        case PINNED_SOURCE_GIPHY:
            strlcpy(shim.giphy.giphy_id, entry.source_id, sizeof(shim.giphy.giphy_id));
            break;
        case PINNED_SOURCE_INSTITUTION: {
            shim.museum.museum_id = entry.museum_id;
            /* source_id is "<museum_id>:<iiif_key>"; strip the prefix. */
            const char *colon = strchr(entry.source_id, ':');
            const char *key = colon ? colon + 1 : entry.source_id;
            strlcpy(shim.museum.iiif_key, key, sizeof(shim.museum.iiif_key));
            break;
        }
        case PINNED_SOURCE_MAKAPIX: {
            /* UUID string -> 16 bytes */
            const char *s = entry.source_id;
            for (int i = 0; i < 16 && *s; i++) {
                while (*s == '-') s++;
                if (!s[0] || !s[1]) break;
                unsigned int v;
                sscanf(s, "%2x", &v);
                shim.makapix.storage_key_uuid[i] = (uint8_t)v;
                s += 2;
            }
            break;
        }
        default:
            send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE\"}");
            return ESP_OK;
    }

    char path[256];
    err = pin_list_build_artwork_path(slug, &shim, path, sizeof(path));
    if (err != ESP_OK) { send_pin_err(req, err, "path"); return ESP_OK; }

    struct stat st;
    if (stat(path, &st) != 0) {
        send_json(req, 404, "{\"ok\":false,\"code\":\"FILE_MISSING\"}");
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        send_json(req, 500, "{\"ok\":false,\"code\":\"OPEN_FAIL\"}");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, get_mime_type(path));

    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  POST handlers                                                            */
/* ------------------------------------------------------------------------- */

static esp_err_t h_post_create(httpd_req_t *req)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }
    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"code\":\"READ_BODY\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }
    cJSON *name_j = cJSON_GetObjectItem(root, "name");
    if (!name_j || !cJSON_IsString(name_j) || cJSON_GetStringValue(name_j)[0] == '\0') {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }
    char slug[PIN_LIST_SLUG_LEN];
    esp_err_t err = pin_lists_create(cJSON_GetStringValue(name_j), slug);
    cJSON_Delete(root);
    if (err != ESP_OK) { send_pin_err(req, err, "create"); return ESP_OK; }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"slug\":\"%s\"}", slug);
    send_json(req, 200, resp);
    return ESP_OK;
}

static esp_err_t h_post_rename(httpd_req_t *req, const char *slug)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }
    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"code\":\"READ_BODY\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }
    cJSON *name_j = cJSON_GetObjectItem(root, "name");
    if (!name_j || !cJSON_IsString(name_j) || cJSON_GetStringValue(name_j)[0] == '\0') {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"INVALID_NAME\"}");
        return ESP_OK;
    }
    esp_err_t err = pin_lists_rename(slug, cJSON_GetStringValue(name_j));
    cJSON_Delete(root);
    if (err != ESP_OK) { send_pin_err(req, err, "rename"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_post_set_active(httpd_req_t *req, const char *slug)
{
    esp_err_t err = pin_lists_set_active(slug);
    if (err != ESP_OK) { send_pin_err(req, err, "set_active"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/pin-lists/{slug}/play
 * Switches playback to the given pinned list as a first-class channel.
 * Persists the active-playset name as "channel_pinned_{slug}" so the
 * pill bar can highlight the active list across polls. */
static esp_err_t h_post_play(httpd_req_t *req, const char *slug)
{
    esp_err_t err = play_scheduler_play_pinned_channel(slug);
    if (err != ESP_OK) { send_pin_err(req, err, "play"); return ESP_OK; }
    char playset_name[40];
    snprintf(playset_name, sizeof(playset_name), "channel_pinned_%s", slug);
    p3a_state_set_active_playset(playset_name);
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/pin-lists/{slug}/items — raw pin (phase 2 testing surface).
 * Body fields (all sources):
 *   source            string  ("makapix"|"giphy"|"museum")
 *   source_id         string  (UUID, giphy_id, or iiif_key composite)
 *   original_post_id  number  (int32)
 *   extension         string  ("webp"|"gif"|"png"|"jpg")
 *   pinned_at         number  (unix s; defaults to now)
 *   original_created_at  number (unix s; optional)
 *   title             string  (optional)
 *   creator           string  (optional)
 *   src_artwork_path  string  (absolute path on SD card to copy from)
 *   museum_id         number  (institution only)
 *   storage_key_uuid  string  (makapix only, 36-char hyphenated)
 *   giphy_id          string  (giphy only)
 *   iiif_key          string  (museum only)
 */
static esp_err_t h_post_pin_raw(httpd_req_t *req, const char *slug)
{
    if (!ensure_json_content(req)) {
        send_json(req, 415, "{\"ok\":false,\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }
    int err_status;
    size_t len;
    char *body = recv_body_json(req, &len, &err_status);
    if (!body) {
        send_json(req, err_status ? err_status : 500, "{\"ok\":false,\"code\":\"READ_BODY\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"INVALID_JSON\"}");
        return ESP_OK;
    }

    cJSON *j_source = cJSON_GetObjectItem(root, "source");
    cJSON *j_src_id = cJSON_GetObjectItem(root, "source_id");
    cJSON *j_origpid = cJSON_GetObjectItem(root, "original_post_id");
    cJSON *j_ext = cJSON_GetObjectItem(root, "extension");
    cJSON *j_path = cJSON_GetObjectItem(root, "src_artwork_path");
    cJSON *j_title = cJSON_GetObjectItem(root, "title");
    cJSON *j_creator = cJSON_GetObjectItem(root, "creator");
    cJSON *j_pinned_at = cJSON_GetObjectItem(root, "pinned_at");
    cJSON *j_orig_created = cJSON_GetObjectItem(root, "original_created_at");
    cJSON *j_museum_id = cJSON_GetObjectItem(root, "museum_id");
    cJSON *j_uuid = cJSON_GetObjectItem(root, "storage_key_uuid");
    cJSON *j_giphy = cJSON_GetObjectItem(root, "giphy_id");
    cJSON *j_iiif = cJSON_GetObjectItem(root, "iiif_key");

    if (!j_source || !cJSON_IsString(j_source) ||
        !j_src_id || !cJSON_IsString(j_src_id) ||
        !j_path   || !cJSON_IsString(j_path)) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"MISSING_FIELDS\"}");
        return ESP_OK;
    }
    pinned_source_t src = source_from_string(cJSON_GetStringValue(j_source));
    if (src == PINNED_SOURCE_NONE) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE\"}");
        return ESP_OK;
    }
    int ext = -1;
    if (j_ext && cJSON_IsString(j_ext)) {
        ext = extension_from_string(cJSON_GetStringValue(j_ext));
    }
    if (ext < 0) {
        cJSON_Delete(root);
        send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_EXTENSION\"}");
        return ESP_OK;
    }

    pinned_order_entry_t order = {0};
    pinned_entry_file_t file = {0};

    int32_t original_post_id = (j_origpid && cJSON_IsNumber(j_origpid))
                               ? (int32_t)cJSON_GetNumberValue(j_origpid) : 0;

    order.source = (uint8_t)src;
    order.extension = (uint8_t)ext;
    order.pinned_at = (j_pinned_at && cJSON_IsNumber(j_pinned_at))
                       ? (uint32_t)cJSON_GetNumberValue(j_pinned_at) : 0;
    file.magic = PINNED_ENTRY_MAGIC;
    file.version = PINNED_FORMAT_VERSION;
    file.source = (uint8_t)src;
    file.extension = (uint8_t)ext;
    file.pinned_at = order.pinned_at;
    file.original_created_at = (j_orig_created && cJSON_IsNumber(j_orig_created))
                                ? (uint32_t)cJSON_GetNumberValue(j_orig_created) : 0;
    strlcpy(file.source_id, cJSON_GetStringValue(j_src_id), sizeof(file.source_id));
    if (j_title && cJSON_IsString(j_title)) {
        strlcpy(file.title, cJSON_GetStringValue(j_title), sizeof(file.title));
    }
    if (j_creator && cJSON_IsString(j_creator)) {
        strlcpy(file.creator, cJSON_GetStringValue(j_creator), sizeof(file.creator));
    }
    if (j_museum_id && cJSON_IsNumber(j_museum_id)) {
        file.museum_id = (uint16_t)cJSON_GetNumberValue(j_museum_id);
    }

    /* Source-specific order-entry variant population + post_id synthesis
       when the caller omitted original_post_id (matches the native channels'
       DJB2 conventions so pinned playback's post_id collides with the same
       artwork's native-channel post_id). */
    switch (src) {
        case PINNED_SOURCE_MAKAPIX: {
            const char *uuid = j_uuid && cJSON_IsString(j_uuid)
                               ? cJSON_GetStringValue(j_uuid)
                               : cJSON_GetStringValue(j_src_id);
            const char *s = uuid;
            for (int i = 0; i < 16 && *s; i++) {
                while (*s == '-') s++;
                if (!s[0] || !s[1]) break;
                unsigned int v;
                if (sscanf(s, "%2x", &v) != 1) break;
                order.makapix.storage_key_uuid[i] = (uint8_t)v;
                s += 2;
            }
            /* Makapix server post_ids cannot be synthesized client-side. */
            if (original_post_id <= 0) {
                cJSON_Delete(root);
                send_json(req, 400, "{\"ok\":false,\"code\":\"MISSING_POST_ID\"}");
                return ESP_OK;
            }
            break;
        }
        case PINNED_SOURCE_GIPHY: {
            const char *gid = j_giphy && cJSON_IsString(j_giphy)
                              ? cJSON_GetStringValue(j_giphy)
                              : cJSON_GetStringValue(j_src_id);
            strlcpy(order.giphy.giphy_id, gid, sizeof(order.giphy.giphy_id));
            if (original_post_id <= 0) {
                original_post_id = giphy_id_to_post_id(gid);
            }
            break;
        }
        case PINNED_SOURCE_INSTITUTION: {
            order.museum.museum_id = file.museum_id;
            const char *iiif = j_iiif && cJSON_IsString(j_iiif)
                               ? cJSON_GetStringValue(j_iiif)
                               : cJSON_GetStringValue(j_src_id);
            strlcpy(order.museum.iiif_key, iiif, sizeof(order.museum.iiif_key));
            if (original_post_id <= 0) {
                static const char *museum_names[] = {
                    "artic", "rijks", "vam", "wellcome", "smk", "loc"
                };
                if (file.museum_id < (sizeof(museum_names) / sizeof(museum_names[0]))) {
                    original_post_id = art_institution_compute_post_id(
                        museum_names[file.museum_id], iiif);
                }
            }
            break;
        }
        default:
            break;
    }

    order.post_id = original_post_id;
    file.post_id = original_post_id;
    file.original_post_id = original_post_id;

    char src_path[256];
    strlcpy(src_path, cJSON_GetStringValue(j_path), sizeof(src_path));
    cJSON_Delete(root);

    esp_err_t err = pin_list_pin(slug, &order, &file, src_path);
    if (err != ESP_OK) { send_pin_err(req, err, "pin"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  DELETE handlers                                                          */
/* ------------------------------------------------------------------------- */

static esp_err_t h_delete_list(httpd_req_t *req, const char *slug)
{
    esp_err_t err = pin_lists_delete(slug);
    if (err != ESP_OK) { send_pin_err(req, err, "delete"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_delete_item(httpd_req_t *req, const char *slug,
                               pinned_source_t src, const char *source_id)
{
    esp_err_t err = pin_list_unpin(slug, src, source_id);
    if (err != ESP_OK) { send_pin_err(req, err, "unpin"); return ESP_OK; }
    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Top-level dispatchers (called from http_api.c routers)                   */
/* ------------------------------------------------------------------------- */

esp_err_t h_pinned_route_get(httpd_req_t *req)
{
    char slug[PIN_LIST_SLUG_LEN] = {0};
    char remainder[160] = {0};
    if (split_uri(req->uri, slug, remainder, sizeof(remainder)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_PATH\"}");
        return ESP_OK;
    }

    /* /api/pin-lists */
    if (slug[0] == '\0') return h_get_collection(req);

    /* /api/pin-lists/{slug} */
    if (remainder[0] == '\0') return h_get_list_info(req, slug);

    /* /api/pin-lists/{slug}/items */
    if (strcmp(remainder, "items") == 0) {
        return h_get_items(req, slug);
    }

    /* /api/pin-lists/{slug}/items/{source}/{source_id}[/local] */
    if (strncmp(remainder, "items/", 6) == 0) {
        char src_str[16] = {0};
        char source_id[PINNED_SOURCE_ID_MAX] = {0};
        bool local = false;
        const char *r = remainder + 6;
        const char *slash = strchr(r, '/');
        if (!slash) { send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_PATH\"}"); return ESP_OK; }
        size_t slen = (size_t)(slash - r);
        if (slen >= sizeof(src_str)) { send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE\"}"); return ESP_OK; }
        memcpy(src_str, r, slen); src_str[slen] = '\0';
        r = slash + 1;
        const char *slash2 = strchr(r, '/');
        size_t slen2 = slash2 ? (size_t)(slash2 - r) : strlen(r);
        if (slen2 >= sizeof(source_id)) { send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE_ID\"}"); return ESP_OK; }
        memcpy(source_id, r, slen2); source_id[slen2] = '\0';
        url_decode_in_place(source_id);
        if (slash2 && strcmp(slash2 + 1, "local") == 0) local = true;

        pinned_source_t src = source_from_string(src_str);
        if (src == PINNED_SOURCE_NONE) {
            send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE\"}");
            return ESP_OK;
        }
        if (local) return h_get_item_local(req, slug, src, source_id);
        return h_get_item(req, slug, src, source_id);
    }

    send_json(req, 404, "{\"ok\":false,\"code\":\"NOT_FOUND\"}");
    return ESP_OK;
}

esp_err_t h_pinned_route_post(httpd_req_t *req)
{
    char slug[PIN_LIST_SLUG_LEN] = {0};
    char remainder[160] = {0};
    if (split_uri(req->uri, slug, remainder, sizeof(remainder)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_PATH\"}");
        return ESP_OK;
    }

    /* POST /api/pin-lists → create */
    if (slug[0] == '\0') return h_post_create(req);

    /* POST /api/pin-lists/{slug}/rename */
    if (strcmp(remainder, "rename") == 0) return h_post_rename(req, slug);

    /* POST /api/pin-lists/{slug}/active */
    if (strcmp(remainder, "active") == 0) return h_post_set_active(req, slug);

    /* POST /api/pin-lists/{slug}/play */
    if (strcmp(remainder, "play") == 0) return h_post_play(req, slug);

    /* POST /api/pin-lists/{slug}/items */
    if (strcmp(remainder, "items") == 0) return h_post_pin_raw(req, slug);

    send_json(req, 404, "{\"ok\":false,\"code\":\"NOT_FOUND\"}");
    return ESP_OK;
}

esp_err_t h_pinned_route_delete(httpd_req_t *req)
{
    char slug[PIN_LIST_SLUG_LEN] = {0};
    char remainder[160] = {0};
    if (split_uri(req->uri, slug, remainder, sizeof(remainder)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_PATH\"}");
        return ESP_OK;
    }

    /* DELETE /api/pin-lists/{slug} */
    if (slug[0] != '\0' && remainder[0] == '\0') return h_delete_list(req, slug);

    /* DELETE /api/pin-lists/{slug}/items/{source}/{source_id} */
    if (strncmp(remainder, "items/", 6) == 0) {
        char src_str[16] = {0};
        char source_id[PINNED_SOURCE_ID_MAX] = {0};
        const char *r = remainder + 6;
        const char *slash = strchr(r, '/');
        if (!slash) { send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_PATH\"}"); return ESP_OK; }
        size_t slen = (size_t)(slash - r);
        if (slen >= sizeof(src_str)) { send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE\"}"); return ESP_OK; }
        memcpy(src_str, r, slen); src_str[slen] = '\0';
        r = slash + 1;
        size_t slen2 = strlen(r);
        if (slen2 >= sizeof(source_id)) { send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE_ID\"}"); return ESP_OK; }
        strlcpy(source_id, r, sizeof(source_id));
        url_decode_in_place(source_id);

        pinned_source_t src = source_from_string(src_str);
        if (src == PINNED_SOURCE_NONE) {
            send_json(req, 400, "{\"ok\":false,\"code\":\"BAD_SOURCE\"}");
            return ESP_OK;
        }
        return h_delete_item(req, slug, src, source_id);
    }

    send_json(req, 404, "{\"ok\":false,\"code\":\"NOT_FOUND\"}");
    return ESP_OK;
}
