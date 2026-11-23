// SPDX-License-Identifier: Apache-2.0
// The player over HTTP: what is playing, next and back, playsets, and
// uploading a file.
//
//   GET    /api/v2/player                 what is playing, the active playset's channels, counters
//   POST   /api/v2/player/next            play the next artwork
//   POST   /api/v2/player/back            play the one before
//   GET    /api/v2/player/playset         the active playset
//   PUT    /api/v2/player/playset         make a playset active: a whole playset, or {"saved": "<name>"}
//   GET    /api/v2/playsets               the saved playsets
//   GET    /api/v2/playsets/{name}        one of them
//   PUT    /api/v2/playsets/{name}        save one (the body's "name", if given, must agree)
//   DELETE /api/v2/playsets/{name}
//   POST   /api/v2/files?name=<file>      upload a picture into the user's folder; the body is the file
//
// Every handler calls the scheduler or the playset store and says what they
// said. The commands a Makapix message can give (M4.5) are the same calls.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "fs_atomic.h"
#include "net_internal.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_limits.h"
#include "p3a_storage.h"
#include "play_scheduler.h"
#include "playset.h"
#include "playset_store.h"
#include "psram_alloc.h"
#include "sd_path.h"
#include "source_local.h"

static const char *const s_channel_states[] = {"waiting", "listing", "ready", "failed", "no_source", "not_loaded"};

// A record as the API shows it: where it is from and what it is called there.
static cJSON *record_json(const p3a_record_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "source", p3a_source_name(r->source));
    char text[40];
    switch (r->source) {
        case P3A_SOURCE_LOCAL:
            cJSON_AddStringToObject(o, "name", r->identity.local.filename);
            break;
        case P3A_SOURCE_GIPHY:
            cJSON_AddStringToObject(o, "id", r->identity.giphy.id);
            break;
        case P3A_SOURCE_KLIPY:
            snprintf(text, sizeof(text), "%llu", (unsigned long long)r->identity.klipy.id);
            cJSON_AddStringToObject(o, "id", text);
            break;
        case P3A_SOURCE_MUSEUM:
            cJSON_AddStringToObject(o, "id", r->identity.museum.iiif_key);
            break;
        case P3A_SOURCE_MAKAPIX:
            cJSON_AddNumberToObject(o, "post_id", r->id32);
            break;
        default:
            break;
    }
    if (r->width != 0) {
        cJSON_AddNumberToObject(o, "width", r->width);
        cJSON_AddNumberToObject(o, "height", r->height);
    }
    return o;
}

static void get_player(p3a_http_request_t *req)
{
    play_scheduler_status_t *st = psram_calloc(1, sizeof(*st));
    if (st == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the player's state");
        return;
    }
    play_scheduler_get_status(st);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "playing", st->playing);
    cJSON_AddBoolToObject(data, "loading", st->loading);
    if (st->playing) {
        cJSON_AddItemToObject(data, "current", record_json(&st->current));
    }
    cJSON_AddStringToObject(data, "playset", st->playset);
    cJSON *channels = cJSON_AddArrayToObject(data, "channels");
    for (uint32_t i = 0; i < st->channel_count; i++) {
        const play_channel_status_t *c = &st->channels[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", c->id);
        cJSON_AddStringToObject(item, "source", playset_source_name(c->source));
        cJSON_AddStringToObject(item, "state", c->state < 6 ? s_channel_states[c->state] : "unknown");
        cJSON_AddNumberToObject(item, "records", c->records);
        cJSON_AddNumberToObject(item, "available", c->available);
        if (c->error != ESP_OK) {
            cJSON_AddStringToObject(item, "error", p3a_err_to_name(c->error));
        }
        cJSON_AddItemToArray(channels, item);
    }
    cJSON *history = cJSON_AddObjectToObject(data, "history");
    cJSON_AddNumberToObject(history, "count", st->history_count);
    cJSON_AddNumberToObject(history, "position", st->history_position);
    cJSON *counters = cJSON_AddObjectToObject(data, "counters");
    cJSON_AddNumberToObject(counters, "picks", st->picks);
    cJSON_AddNumberToObject(counters, "shown", st->shown);
    cJSON_AddNumberToObject(counters, "failed", st->failed);
    if (st->last_error != ESP_OK) {
        cJSON_AddStringToObject(data, "last_error", p3a_err_to_name(st->last_error));
    }
    free(st);
    p3a_http_ok(req, data);
}

static void answer_step(p3a_http_request_t *req, esp_err_t err, const char *nothing)
{
    if (err == ESP_ERR_NOT_FOUND) {
        p3a_http_fail(req, 409, "NOTHING_TO_PLAY", "%s", nothing);
    } else if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "asking the player");
    } else {
        p3a_http_ok(req, NULL);  // asked for; GET /api/v2/player says what became of it
    }
}

static void post_next(p3a_http_request_t *req)
{
    answer_step(req, play_scheduler_next(), "no channel of the active playset has anything to play yet");
}

static void post_back(p3a_http_request_t *req)
{
    answer_step(req, play_scheduler_back(), "there is nothing earlier in the history");
}

// --- playsets -------------------------------------------------------------------

static void get_active_playset(p3a_http_request_t *req)
{
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    if (playset == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the active playset");
        return;
    }
    play_scheduler_get_playset(playset);
    cJSON *data = playset_to_json(playset);
    free(playset);
    if (data == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the active playset");
        return;
    }
    p3a_http_ok(req, data);
}

static void put_active_playset(p3a_http_request_t *req)
{
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    if (playset == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "activating a playset");
        return;
    }
    const cJSON *body = p3a_http_body(req);
    const cJSON *saved = cJSON_GetObjectItemCaseSensitive(body, "saved");
    char why[120] = "";
    esp_err_t err;
    if (saved != NULL) {
        if (!cJSON_IsString(saved) || cJSON_GetArraySize(body) != 1) {
            free(playset);
            p3a_http_fail(req, 400, "INVALID_PLAYSET", "give either a whole playset or only {\"saved\": name}");
            return;
        }
        err = playset_store_load(saved->valuestring, playset);
        if (err != ESP_OK) {
            free(playset);
            p3a_http_fail_err(req, err, "loading that playset");
            return;
        }
    } else if ((err = playset_from_json(body, playset, why, sizeof(why))) != ESP_OK) {
        free(playset);
        p3a_http_fail(req, 400, "INVALID_PLAYSET", "%s", why);
        return;
    }
    err = play_scheduler_activate(playset);
    free(playset);
    if (err != ESP_OK) {
        // It is active all the same; what failed is keeping it for the next start.
        p3a_http_fail_err(req, err, "saving the active playset (it is active until the next restart)");
        return;
    }
    p3a_http_ok(req, NULL);
}

static void get_playsets(p3a_http_request_t *req)
{
    playset_store_entry_t *entries = psram_calloc(PLAYSET_STORE_MAX, sizeof(playset_store_entry_t));
    uint32_t count = 0, unreadable = 0;
    esp_err_t err = entries ? playset_store_list(entries, PLAYSET_STORE_MAX, &count, &unreadable) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        free(entries);
        p3a_http_fail_err(req, err, "listing the playsets");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(data, "playsets");
    for (uint32_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "channels", entries[i].channel_count);
        cJSON_AddItemToArray(list, item);
    }
    // Files in the folder that could not be read: said, so that a playset
    // that vanished from the list is not a mystery.
    cJSON_AddNumberToObject(data, "unreadable", unreadable);
    cJSON_AddNumberToObject(data, "max", PLAYSET_STORE_MAX);
    free(entries);
    p3a_http_ok(req, data);
}

static bool name_param(p3a_http_request_t *req, char *name, size_t len)
{
    if (p3a_http_param(req, 0, name, len) && name[0] != '\0') {
        return true;
    }
    p3a_http_fail(req, 400, "INVALID_PLAYSET", "a playset's name is 1 to %d characters", PLAYSET_NAME_LEN - 1);
    return false;
}

static void get_playset(p3a_http_request_t *req)
{
    char name[PLAYSET_NAME_LEN];
    if (!name_param(req, name, sizeof(name))) {
        return;
    }
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    esp_err_t err = playset ? playset_store_load(name, playset) : ESP_ERR_NO_MEM;
    cJSON *data = err == ESP_OK ? playset_to_json(playset) : NULL;
    free(playset);
    if (data == NULL) {
        p3a_http_fail_err(req, err != ESP_OK ? err : ESP_ERR_NO_MEM, "reading that playset");
        return;
    }
    p3a_http_ok(req, data);
}

static void put_playset(p3a_http_request_t *req)
{
    char name[PLAYSET_NAME_LEN];
    if (!name_param(req, name, sizeof(name))) {
        return;
    }
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    if (playset == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "saving a playset");
        return;
    }
    char why[120] = "";
    esp_err_t err = playset_from_json(p3a_http_body(req), playset, why, sizeof(why));
    if (err == ESP_OK && playset->name[0] != '\0' && strcmp(playset->name, name) != 0) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(why, sizeof(why), "the body names another playset than the address does");
    }
    if (err != ESP_OK) {
        free(playset);
        p3a_http_fail(req, 400, "INVALID_PLAYSET", "%s", why);
        return;
    }
    strcpy(playset->name, name);
    err = playset_store_save(playset);
    free(playset);
    if (err == ESP_ERR_NO_MEM) {
        p3a_http_fail(req, 409, "TOO_MANY_PLAYSETS", "%d playsets are saved already; delete one first",
                      PLAYSET_STORE_MAX);
    } else if (err == ESP_ERR_INVALID_STATE) {
        p3a_http_fail(req, 409, "UNREADABLE_PLAYSET",
                      "the file kept for this name cannot be read and was left alone; delete the playset to replace it");
    } else if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "saving the playset");
    } else {
        p3a_http_ok(req, NULL);
    }
}

static void delete_playset(p3a_http_request_t *req)
{
    char name[PLAYSET_NAME_LEN];
    if (!name_param(req, name, sizeof(name))) {
        return;
    }
    const esp_err_t err = playset_store_delete(name);
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "deleting that playset");
        return;
    }
    p3a_http_ok(req, NULL);
}

// --- upload ---------------------------------------------------------------------

typedef struct {
    httpd_req_t *raw;
    size_t len;
    esp_err_t receive_err;
} upload_t;

// Streams the request body into the file fs_atomic opened.
static esp_err_t upload_writer(FILE *f, void *ctx)
{
    upload_t *up = ctx;
    char *chunk = psram_malloc(16 * 1024);
    if (chunk == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    int idle = 0;
    for (size_t got = 0; got < up->len && err == ESP_OK;) {
        const size_t want = up->len - got < 16 * 1024 ? up->len - got : 16 * 1024;
        const int n = httpd_req_recv(up->raw, chunk, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++idle < 6) {
            continue;  // a slow sender is given half a minute, not for ever
        }
        if (n <= 0) {
            up->receive_err = ESP_ERR_TIMEOUT;
            err = ESP_ERR_TIMEOUT;
        } else if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
            err = ESP_FAIL;
        } else {
            got += (size_t)n;
            idle = 0;
        }
    }
    free(chunk);
    return err;
}

static void post_file(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    char name[P3A_RECORD_FILENAME_LEN + 8], dir[160], path[320];
    if (!p3a_http_query(req, "name", name, sizeof(name)) || strlen(name) >= P3A_RECORD_FILENAME_LEN) {
        p3a_http_fail(req, 400, "INVALID_NAME", "give the file's name in ?name=, at most %d characters",
                      P3A_RECORD_FILENAME_LEN - 1);
        return;
    }
    // The name as it will be on the card must be the name that was sent:
    // nothing is renamed behind the user's back.
    char clean[sizeof(name)];
    sd_path_sanitize_filename(name, clean, sizeof(clean));
    if (strcmp(clean, name) != 0 || strchr(name, '/') != NULL) {
        p3a_http_fail(req, 400, "INVALID_NAME", "the name has characters a file on the card cannot have");
        return;
    }
    if (source_local_format_of(name) == P3A_FORMAT_UNKNOWN) {
        p3a_http_fail(req, 415, "UNSUPPORTED_TYPE", "the name must end in .webp, .gif, .png, .apng, .jpg, .jpeg or .bmp");
        return;
    }
    const size_t len = raw->content_len;
    if (len == 0) {
        p3a_http_fail(req, 400, "INVALID", "send the file as the request body");
        return;
    }
    if (len > P3A_MAX_ARTWORK_BYTES) {
        p3a_http_fail(req, 413, "TOO_LARGE", "a file may be at most %d MB", P3A_MAX_ARTWORK_BYTES / (1024 * 1024));
        return;
    }
    const p3a_storage_space_t space = p3a_storage_space();
    if (space.valid && space.free_bytes < (uint64_t)len + 8 * 1024 * 1024) {
        p3a_http_fail(req, 507, "CARD_FULL", "the SD card has %u MB free", (unsigned)(space.free_bytes / (1024 * 1024)));
        return;
    }
    esp_err_t err = sd_path_get_animations(dir, sizeof(dir));
    if (err == ESP_OK) {
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        err = sd_path_ensure_parent_dirs(path);
    }
    upload_t up = {.raw = raw, .len = len};
    if (err == ESP_OK) {
        // Written beside its final name and moved into place when complete:
        // a broken upload leaves no half file among the user's pictures.
        err = fs_atomic_write_cb(path, upload_writer, &up, NULL);
    }
    if (err != ESP_OK) {
        if (up.receive_err != ESP_OK) {
            p3a_http_fail(req, 400, "READ_BODY", "the file did not arrive completely");
        } else {
            p3a_http_fail_err(req, err, "saving the file");
        }
        return;
    }
    (void)play_scheduler_refresh_source(P3A_SOURCE_LOCAL);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name);  // through the JSON builder: quotes and all (audit M7)
    cJSON_AddNumberToObject(data, "bytes", (double)len);
    p3a_http_ok(req, data);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/player", get_player},
    {P3A_HTTP_POST, 0, "/api/v2/player/next", post_next},
    {P3A_HTTP_POST, 0, "/api/v2/player/back", post_back},
    {P3A_HTTP_GET, 0, "/api/v2/player/playset", get_active_playset},
    {P3A_HTTP_PUT, P3A_HTTP_BODY_JSON, "/api/v2/player/playset", put_active_playset},
    {P3A_HTTP_GET, 0, "/api/v2/playsets", get_playsets},
    {P3A_HTTP_GET, 0, "/api/v2/playsets/{name}", get_playset},
    {P3A_HTTP_PUT, P3A_HTTP_BODY_JSON, "/api/v2/playsets/{name}", put_playset},
    {P3A_HTTP_DELETE, 0, "/api/v2/playsets/{name}", delete_playset},
    {P3A_HTTP_POST, 0, "/api/v2/files", post_file},
};

void net_player_api_register(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
