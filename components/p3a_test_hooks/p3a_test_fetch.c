// SPDX-License-Identifier: Apache-2.0
// Test commands for the HTTP fetcher. Arguments are JSON.
//
// !fetch get {"url": "..."}                 fetches into memory (at most 16 KB); answers the
//                                           result, the status, the size and the first bytes
// !fetch blob {"url": "...", "format": n}   downloads to the card, hashing as it writes, and
//                                           takes the file into the blob store; answers the
//                                           SHA-256 and the blob's reference
//
// Both run on the console's task and answer when the transfer is over.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "http_fetch.h"
#include "p3a_blob.h"
#include "p3a_err.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"
#include "sd_path.h"

#define GET_MAX 16384

static void hex(const uint8_t *bytes, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", bytes[i]);
    }
}

static void reply(cJSON *out)
{
    char *text = cJSON_PrintUnformatted(out);
    p3a_test_reply_ok(text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(out);
}

static void cmd_fetch(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    const bool get = strncmp(args, "get ", 4) == 0, blob = strncmp(args, "blob ", 5) == 0;
    cJSON *json = (get || blob) ? cJSON_Parse(args + (get ? 4 : 5)) : NULL;
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");
    if (!cJSON_IsString(url)) {
        cJSON_Delete(json);
        p3a_test_reply_err("fetch get {\"url\":...} | fetch blob {\"url\":...,\"format\":n}");
        return;
    }
    const http_fetch_request_t req = {.url = url->valuestring, .user_agent = "p3a-test", .hash_file = blob,
                                      .max_size = 4 * 1024 * 1024};
    http_fetch_result_t result = {0};
    cJSON *out = cJSON_CreateObject();
    esp_err_t err;
    if (get) {
        char *body = psram_malloc(GET_MAX);
        size_t len = 0;
        err = body ? http_fetch_to_buffer(&req, body, GET_MAX, &len, &result) : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            body[len < 200 ? len : 200] = '\0';
            cJSON_AddStringToObject(out, "start", body);
        }
        free(body);
    } else {
        const cJSON *format = cJSON_GetObjectItemCaseSensitive(json, "format");
        char dir[128], path[192];
        sd_path_get_temporary(dir, sizeof(dir));
        snprintf(path, sizeof(path), "%s/test-fetch.part", dir);
        http_fetch_request_t file_req = req;
        file_req.leave_temp = true;  // the blob store moves it into place
        err = http_fetch_to_file(&file_req, path, &result);
        if (err == ESP_OK && result.has_sha256) {
            uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
            bool already = false;
            err = p3a_blob_adopt_hashed(path, (p3a_format_t)(cJSON_IsNumber(format) ? format->valueint : 0), result.sha256,
                                        ref, &already);
            char text[65];
            hex(result.sha256, 32, text);
            cJSON_AddStringToObject(out, "sha256", text);
            if (err == ESP_OK) {
                hex(ref, sizeof(ref), text);
                cJSON_AddStringToObject(out, "ref", text);
                cJSON_AddBoolToObject(out, "already", already);
            }
        }
        if (err != ESP_OK) {
            unlink(path);
        }
    }
    cJSON_AddStringToObject(out, "result", err == ESP_OK ? "ok" : p3a_err_to_name(err));
    cJSON_AddNumberToObject(out, "status", result.http_status);
    cJSON_AddNumberToObject(out, "bytes", (double)result.bytes);
    cJSON_AddNumberToObject(out, "attempts", result.attempts);
    cJSON_Delete(json);
    reply(out);
}

const p3a_test_command_t p3a_test_fetch_commands[] = {
    {"fetch", cmd_fetch},
};
const size_t p3a_test_fetch_command_count = sizeof(p3a_test_fetch_commands) / sizeof(p3a_test_fetch_commands[0]);
