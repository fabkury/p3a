// SPDX-License-Identifier: Apache-2.0
// Test commands for the scheduler.
//
// !sched status              the active playset's channels, what is playing, the history, counters
// !sched next | back
// !sched activate {playset}
// !sched refresh <source>    lists that source's channels again
// !sched auto on|off         whether the device moves on by itself. Off from boot in a test
//                            build, so that tests find on the panel what they put there
// !sched pause on|off
//
//   POST /api/v2/test/local?name=<file name>   body: a file, written into the user's
//                                              animations folder (no body: the file is removed)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "fs_atomic.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_test_internal.h"
#include "play_scheduler.h"
#include "playset.h"
#include "psram_alloc.h"
#include "sd_path.h"

static bool s_auto = true;  // whether this file's hold is off

static const char *const s_states[] = {"waiting", "listing", "ready", "failed", "no_source", "not_loaded"};

static void reply_json(cJSON *out)
{
    char *text = cJSON_PrintUnformatted(out);
    p3a_test_reply_ok(text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(out);
}

static void reply_result(esp_err_t err)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "result", err == ESP_OK ? "ok" : p3a_err_to_name(err));
    reply_json(out);
}

static void cmd_status(void)
{
    play_scheduler_status_t *st = psram_calloc(1, sizeof(*st));
    if (st == NULL) {
        p3a_test_reply_err("no memory");
        return;
    }
    play_scheduler_get_status(st);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "playset", st->playset);
    cJSON *channels = cJSON_AddArrayToObject(out, "channels");
    for (uint32_t i = 0; i < st->channel_count; i++) {
        const play_channel_status_t *c = &st->channels[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", c->id);
        cJSON_AddStringToObject(item, "source", playset_source_name(c->source));
        cJSON_AddStringToObject(item, "state", c->state < 6 ? s_states[c->state] : "?");
        cJSON_AddNumberToObject(item, "records", c->records);
        cJSON_AddNumberToObject(item, "available", c->available);
        if (c->error != ESP_OK) {
            cJSON_AddStringToObject(item, "error", p3a_err_to_name(c->error));
        }
        cJSON_AddItemToArray(channels, item);
    }
    cJSON_AddBoolToObject(out, "playing", st->playing);
    cJSON_AddBoolToObject(out, "loading", st->loading);
    if (st->playing) {
        cJSON_AddStringToObject(out, "current_source", p3a_source_name(st->current.source));
        cJSON_AddStringToObject(out, "current",
                                st->current.source == P3A_SOURCE_LOCAL ? st->current.identity.local.filename : "");
    }
    cJSON_AddNumberToObject(out, "history", st->history_count);
    cJSON_AddNumberToObject(out, "position", st->history_position);
    cJSON_AddNumberToObject(out, "picks", st->picks);
    cJSON_AddNumberToObject(out, "shown", st->shown);
    cJSON_AddNumberToObject(out, "failed", st->failed);
    cJSON_AddStringToObject(out, "last_error", st->last_error == ESP_OK ? "" : p3a_err_to_name(st->last_error));
    cJSON_AddBoolToObject(out, "paused", st->paused);
    cJSON_AddNumberToObject(out, "advance_in_s", st->advance_in_s);
    cJSON_AddStringToObject(out, "problem", st->problem == ESP_OK ? "" : p3a_err_to_name(st->problem));
    cJSON_AddBoolToObject(out, "auto", s_auto);
    free(st);
    reply_json(out);
}

static void set_auto(bool on)
{
    if (on != s_auto) {
        on ? play_scheduler_release() : play_scheduler_hold();
        s_auto = on;
    }
}

static void cmd_sched(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    unsigned source = 0;
    if (strcmp(args, "status") == 0) {
        cmd_status();
    } else if (strcmp(args, "next") == 0) {
        reply_result(play_scheduler_next());
    } else if (strcmp(args, "back") == 0) {
        reply_result(play_scheduler_back());
    } else if (strcmp(args, "auto on") == 0 || strcmp(args, "auto off") == 0) {
        set_auto(args[6] == 'n');
        reply_result(ESP_OK);
    } else if (strcmp(args, "pause on") == 0 || strcmp(args, "pause off") == 0) {
        reply_result(play_scheduler_set_paused(args[7] == 'n'));
    } else if (sscanf(args, "refresh %u", &source) == 1) {
        reply_result(play_scheduler_refresh_source((uint8_t)source));
    } else if (strncmp(args, "activate ", 9) == 0) {
        playset_t *playset = psram_calloc(1, sizeof(playset_t));
        cJSON *json = cJSON_Parse(args + 9);
        esp_err_t err = (playset && json) ? playset_from_json(json, playset, NULL, 0) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            err = play_scheduler_activate(playset);
        }
        cJSON_Delete(json);
        free(playset);
        reply_result(err);
    } else {
        p3a_test_reply_err("sched status|next|back|activate {playset}|refresh <source>|auto on|off|pause on|off");
    }
}

const p3a_test_command_t p3a_test_sched_commands[] = {
    {"sched", cmd_sched},
};
const size_t p3a_test_sched_command_count = sizeof(p3a_test_sched_commands) / sizeof(p3a_test_sched_commands[0]);

// --- a file into the user's folder ---------------------------------------------

static void post_local(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    char name[128], dir[160], path[300];
    if (!p3a_http_query(req, "name", name, sizeof(name)) || name[0] == '\0' || strchr(name, '/') != NULL ||
        sd_path_get_animations(dir, sizeof(dir)) != ESP_OK) {
        p3a_http_fail(req, 400, "INVALID", "give a file name");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    const size_t len = raw->content_len;
    if (len == 0) {
        unlink(path);
        p3a_http_ok(req, NULL);
        return;
    }
    uint8_t *body = len <= 4 * 1024 * 1024 ? psram_malloc(len) : NULL;
    if (body == NULL) {
        p3a_http_fail(req, 413, "TOO_LARGE", "at most 4 MiB");
        return;
    }
    for (size_t got = 0; got < len;) {
        const int n = httpd_req_recv(raw, (char *)body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(body);
            p3a_http_fail(req, 400, "READ_BODY", "the file did not arrive");
            return;
        }
        got += (size_t)n;
    }
    esp_err_t err = sd_path_ensure_parent_dirs(path);
    if (err == ESP_OK) {
        err = fs_atomic_write(path, body, len, NULL);
    }
    free(body);
    if (err != ESP_OK) {
        p3a_http_fail_err(req, err, "writing the file");
        return;
    }
    p3a_http_ok(req, NULL);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_POST, P3A_HTTP_RAW, "/api/v2/test/local", post_local},
};

void p3a_test_sched_start(void)
{
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
    set_auto(false);
}
