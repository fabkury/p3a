// SPDX-License-Identifier: Apache-2.0
// The player over HTTP: what is playing, next and back, playsets, and
// uploading a file.
//
//   GET    /api/v2/player                 what is playing, the active playset's channels, counters
//   POST   /api/v2/player/next            play the next artwork
//   POST   /api/v2/player/back            play the one before
//   POST   /api/v2/player/pause           black panel, backlight off, no moving on
//   POST   /api/v2/player/resume
//   POST   /api/v2/player/refresh         list every channel of the active playset again, now
//   POST   /api/v2/player/like            what the artwork's source makes of a like (Makapix: a
//   POST   /api/v2/player/unlike          thumbs-up from the owner; Giphy: a click); answers the reaction
//   POST   /api/v2/player/show            {"url"}: fetch the picture at that address, keep it among the
//                                         user's files and play it; answers 202 with the job
//   GET    /api/v2/player/playset         the active playset
//   PUT    /api/v2/player/playset         make a playset active: a whole playset, or {"saved": "<name>"}
//   GET    /api/v2/playsets               the saved playsets
//   GET    /api/v2/playsets/{name}        one of them
//   PUT    /api/v2/playsets/{name}        save one (the body's "name", if given, must agree)
//   DELETE /api/v2/playsets/{name}
//   POST   /api/v2/files?name=<file>      upload a picture into the user's folder; the body is the file.
//                                         It plays at once
//
// Every handler calls the scheduler or the playset store and says what they
// said. The commands a Makapix message can give (M4.5) are the same calls.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "fs_atomic.h"
#include "net_internal.h"
#include "p3a_blob.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_limits.h"
#include "p3a_pins.h"
#include "p3a_settings.h"
#include "p3a_source.h"
#include "p3a_storage.h"
#include "play_scheduler.h"
#include "show_url.h"
#include "playset.h"
#include "playset_store.h"
#include "psram_alloc.h"
#include "sd_path.h"
#include "source_local.h"
#include "source_museum.h"

static const char *const s_channel_states[] = {"waiting", "listing", "ready", "failed", "no_source", "not_loaded"};

// A record as the API shows it: where it is from and what it is called there.
cJSON *net_record_json(const p3a_record_t *r)
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
        case P3A_SOURCE_MUSEUM: {
            cJSON_AddStringToObject(o, "id", r->identity.museum.iiif_key);
            // Which museum: the page asks that museum what the artwork is.
            const char *museum = source_museum_id(r->identity.museum.museum_id);
            if (museum != NULL) {
                cJSON_AddStringToObject(o, "museum", museum);
            }
            break;
        }
        case P3A_SOURCE_MAKAPIX: {
            cJSON_AddNumberToObject(o, "post_id", r->id32);
            // The storage key, as the site spells it: what its public pages know the artwork by.
            char key[37];
            const uint8_t *u = r->identity.makapix.uuid;
            snprintf(key, sizeof(key), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1],
                     u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            cJSON_AddStringToObject(o, "storage_key", key);
            break;
        }
        default:
            break;
    }
    const char *extension = p3a_blob_extension((p3a_format_t)r->format);
    if (extension != NULL) {
        cJSON_AddStringToObject(o, "format", extension + 1);  // "gif", "webp", "png", "jpg", "bmp"; absent until known
    }
    if (r->width != 0) {
        cJSON_AddNumberToObject(o, "width", r->width);
        cJSON_AddNumberToObject(o, "height", r->height);
    }
    // Where the page gets the picture: from this device (net_media_api.c).
    char file[P3A_RECORD_FILENAME_LEN * 3 + 24];
    if (r->source == P3A_SOURCE_LOCAL) {
        // The name goes into an address: everything but letters, digits and "-._~" escaped.
        size_t at = (size_t)snprintf(file, sizeof(file), "/api/v2/files/");
        for (const unsigned char *p = (const unsigned char *)r->identity.local.filename; *p != 0 && at + 4 < sizeof(file); p++) {
            const bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                               *p == '-' || *p == '.' || *p == '_' || *p == '~';
            at += (size_t)snprintf(file + at, sizeof(file) - at, plain ? "%c" : "%%%02X", *p);
        }
        cJSON_AddStringToObject(o, "file", file);
    } else if (p3a_record_has_blob(r) && extension != NULL) {
        size_t at = (size_t)snprintf(file, sizeof(file), "/api/v2/blobs/");
        for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
            at += (size_t)snprintf(file + at, sizeof(file) - at, "%02x", r->blob_ref[i]);
        }
        snprintf(file + at, sizeof(file) - at, "%s", extension);
        cJSON_AddStringToObject(o, "file", file);
    }
    return o;
}

// Whether the artwork on the screen is in the active pin list. Asking reads
// the list from the card, and the page asks for the player's state every few
// seconds: the answer is kept until the lists or the artwork change.
static bool current_is_pinned(const p3a_record_t *current)
{
    static uint32_t revision = UINT32_MAX;
    static p3a_record_t asked;
    static bool pinned;
    const uint32_t now = p3a_pins_revision();
    if (now != revision || memcmp(&asked, current, sizeof(asked)) != 0) {
        revision = now;
        asked = *current;
        pinned = p3a_pins_is_pinned(NULL, current);
    }
    return pinned;
}

static void get_player(p3a_http_request_t *req)
{
    play_scheduler_status_t *st = psram_calloc(1, sizeof(*st));
    playset_t *playset = psram_calloc(1, sizeof(*playset));
    static p3a_settings_t settings;  // large; the server has one task
    if (st == NULL || playset == NULL) {
        free(st);
        free(playset);
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "reading the player's state");
        return;
    }
    play_scheduler_get_status(st);
    play_scheduler_get_playset(playset);
    p3a_settings_get(&settings);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "playing", st->playing);
    cJSON_AddBoolToObject(data, "loading", st->loading);
    cJSON_AddBoolToObject(data, "paused", st->paused);
    cJSON_AddBoolToObject(data, "downloading", st->downloading);
    if (st->advance_in_s >= 0) {
        cJSON_AddNumberToObject(data, "advance_in_s", st->advance_in_s);  // absent: it will not move on by itself
    }
    if (st->problem != ESP_OK) {
        // What was asked for and could not be shown; gone once something is.
        cJSON_AddStringToObject(data, "problem", p3a_err_to_name(st->problem));
    }
    if (st->playing) {
        cJSON *current = net_record_json(&st->current);
        cJSON_AddBoolToObject(current, "pinned", current_is_pinned(&st->current));  // in the active list
        cJSON_AddItemToObject(data, "current", current);
    }
    cJSON_AddStringToObject(data, "playset", st->playset);
    cJSON_AddBoolToObject(data, "shuffle", settings.pick_mode != P3A_PICK_MODE_RECENCY);
    cJSON_AddNumberToObject(data, "pins_revision", p3a_pins_revision());  // changes when the pin lists do
    cJSON *channels = cJSON_AddArrayToObject(data, "channels");
    for (uint32_t i = 0; i < st->channel_count; i++) {
        const play_channel_status_t *c = &st->channels[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", c->id);
        cJSON_AddStringToObject(item, "source", playset_source_name(c->source));
        if (i < playset->count) {
            // What the playset says of it: enough for the page to name the channel.
            const playset_channel_t *spec = &playset->channels[i];
            cJSON_AddStringToObject(item, "kind", spec->kind);
            cJSON_AddStringToObject(item, "identifier", spec->identifier);
            cJSON_AddStringToObject(item, "display_name", spec->display_name);
            cJSON_AddNumberToObject(item, "weight", spec->weight);
        }
        cJSON_AddStringToObject(item, "state", c->state < 6 ? s_channel_states[c->state] : "unknown");
        cJSON_AddNumberToObject(item, "records", c->records);
        cJSON_AddNumberToObject(item, "available", c->available);
        cJSON_AddNumberToObject(item, "listings", c->listings);
        cJSON_AddNumberToObject(item, "refresh_in_s", c->refresh_in_s);
        cJSON_AddNumberToObject(item, "capacity", c->capacity);    // as this playset has it: the channel's own, or its source's
        cJSON_AddNumberToObject(item, "refresh_s", c->refresh_s);
        if (c->remote) {
            cJSON_AddNumberToObject(item, "to_download", c->wanted);
        }
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
    cJSON_AddNumberToObject(counters, "problems", st->problems);
    cJSON_AddNumberToObject(counters, "downloads", st->downloads);
    cJSON_AddNumberToObject(counters, "download_failures", st->download_failures);
    if (st->last_error != ESP_OK) {
        cJSON_AddStringToObject(data, "last_error", p3a_err_to_name(st->last_error));
    }
    free(st);
    free(playset);
    p3a_http_ok(req, data);
}

static void answer_step(p3a_http_request_t *req, esp_err_t err, const char *nothing)
{
    if (err == ESP_ERR_NOT_FOUND) {
        p3a_http_fail(req, 409, "NOTHING_TO_PLAY", "%s", nothing);
    } else if (err == ESP_ERR_INVALID_STATE) {
        p3a_http_fail(req, 409, "PAUSED", "the player is paused; resume it first (POST /api/v2/player/resume)");
    } else if (err == ESP_ERR_NOT_ALLOWED) {
        p3a_http_fail(req, 409, "CARD_IN_USE", "the SD card is in use by a computer or being formatted; the device restarts when that ends");
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

static void post_pause(p3a_http_request_t *req)
{
    const esp_err_t err = play_scheduler_set_paused(true);
    err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "pausing");
}

static void post_resume(p3a_http_request_t *req)
{
    const esp_err_t err = play_scheduler_set_paused(false);
    err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "resuming");
}

static void post_refresh(p3a_http_request_t *req)
{
    const esp_err_t err = play_scheduler_refresh_all();
    err == ESP_OK ? p3a_http_ok(req, NULL) : p3a_http_fail_err(req, err, "asking for fresh listings");
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
    } else if ((err = playset_from_json(body, playset, why, sizeof(why))) != ESP_OK ||
               (err = play_scheduler_check_playset(playset, why, sizeof(why))) != ESP_OK) {
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
    if (err == ESP_OK) {
        err = play_scheduler_check_playset(playset, why, sizeof(why));
    }
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
    if (p3a_storage_is_handed_over()) {
        p3a_http_fail(req, 409, "CARD_IN_USE", "a computer has the SD card over USB; copy the file there, or unplug it first");
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
    // It plays at once, as the person who sent it expects, and the folder is
    // listed again so that it also takes its place in the rotation. While
    // paused it is only listed.
    p3a_record_t record;
    const bool shown = source_local_record(name, (uint32_t)time(NULL), &record) == ESP_OK &&
                       play_scheduler_show(&record) == ESP_OK;
    (void)play_scheduler_refresh_source(P3A_SOURCE_LOCAL);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name);  // through the JSON builder: quotes and all (audit M7)
    cJSON_AddNumberToObject(data, "bytes", (double)len);
    cJSON_AddBoolToObject(data, "playing", shown);
    p3a_http_ok(req, data);
}

// A like, or its taking back, for the artwork on the screen: what a swipe up
// or down does on the device, without the pin.
static void react(p3a_http_request_t *req, bool undo)
{
    static const char *const names[] = {"none", "liked", "unliked", "noted"};
    p3a_record_t record;
    if (!play_scheduler_current(&record)) {
        p3a_http_fail(req, 409, "NOTHING_SHOWN", "no artwork is on the screen");
        return;
    }
    const p3a_source_ops_t *ops = p3a_source_get(record.source);
    p3a_reaction_t reaction = ops != NULL && ops->react != NULL ? ops->react(&record, undo) : P3A_REACTION_NONE;
    if (reaction > P3A_REACTION_NOTED) {
        reaction = P3A_REACTION_NONE;
    }
    cJSON *data = net_record_json(&record);
    // "none": this artwork's source has no such thing, or cannot now (no key, not registered).
    cJSON_AddStringToObject(data, "reaction", names[reaction]);
    p3a_http_ok(req, data);
}

static void post_like(p3a_http_request_t *req)
{
    react(req, false);
}

static void post_unlike(p3a_http_request_t *req)
{
    react(req, true);
}

// "Show this": a picture at an address, fetched, saved among the user's
// files and played (show_url.h). The answer is the job that does it.
static void post_show(p3a_http_request_t *req)
{
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(p3a_http_body(req), "url");
    if (!cJSON_IsString(url) || !show_url_ok(url->valuestring)) {
        p3a_http_fail(req, 400, "INVALID_URL", "give \"url\": an http:// or https:// address of a picture, under %d characters",
                      SHOW_URL_MAX_LEN);
        return;
    }
    if (play_scheduler_is_paused()) {
        p3a_http_fail(req, 409, "PAUSED", "the player is paused; resume it first (POST /api/v2/player/resume)");
        return;
    }
    if (p3a_storage_is_handed_over()) {
        p3a_http_fail(req, 409, "CARD_IN_USE", "a computer has the SD card over USB; unplug it first");
        return;
    }
    if (!p3a_net_is_up()) {
        p3a_http_fail(req, 409, "OFFLINE", "the device has no network to fetch it over");
        return;
    }
    p3a_job_id_t job = P3A_JOB_ID_NONE;
    const esp_err_t err = play_scheduler_show_url(url->valuestring, &job);
    err == ESP_OK ? p3a_http_accepted(req, job) : p3a_http_fail_err(req, err, "starting the fetch");
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_GET, 0, "/api/v2/player", get_player},
    {P3A_HTTP_POST, 0, "/api/v2/player/next", post_next},
    {P3A_HTTP_POST, 0, "/api/v2/player/back", post_back},
    {P3A_HTTP_POST, 0, "/api/v2/player/pause", post_pause},
    {P3A_HTTP_POST, 0, "/api/v2/player/resume", post_resume},
    {P3A_HTTP_POST, 0, "/api/v2/player/refresh", post_refresh},
    {P3A_HTTP_POST, 0, "/api/v2/player/like", post_like},
    {P3A_HTTP_POST, 0, "/api/v2/player/unlike", post_unlike},
    {P3A_HTTP_POST, P3A_HTTP_BODY_JSON, "/api/v2/player/show", post_show},
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
