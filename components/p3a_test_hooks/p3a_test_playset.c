// SPDX-License-Identifier: Apache-2.0
// Test commands for playsets on the card. Arguments and answers are JSON.
//
// !playset save {playset}            saves it under its name
// !playset load {"name": ...}        answers the playset as JSON
// !playset delete {"name": ...}
// !playset list                      names and channel counts, and how many files could not be read
// !playset active-save {playset}     the active playset
// !playset active-load
// !playset active-forget             removes the active playset's file: at the next boot the device
//                                    is one that was never told what to play
// !playset damage {"name": ...}      flips one byte in that playset's file

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "p3a_err.h"
#include "p3a_test_internal.h"
#include "playset.h"
#include "playset_store.h"
#include "psram_alloc.h"

static void reply_result(esp_err_t err, const char *why)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "result", err == ESP_OK ? "ok" : p3a_err_to_name(err));
    if (why && why[0]) {
        cJSON_AddStringToObject(out, "why", why);
    }
    char *text = cJSON_PrintUnformatted(out);
    p3a_test_reply_ok(text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(out);
}

static void reply_playset(esp_err_t err, const playset_t *playset)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "result", err == ESP_OK ? "ok" : p3a_err_to_name(err));
    if (err == ESP_OK) {
        cJSON_AddItemToObject(out, "playset", playset_to_json(playset));
    }
    char *text = cJSON_PrintUnformatted(out);
    p3a_test_reply_ok(text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(out);
}

// The "name" of a {"name": ...} argument, copied out; false if there is none.
static bool name_arg(const char *json_text, char *name, size_t len)
{
    cJSON *json = cJSON_Parse(json_text);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "name");
    const bool ok = cJSON_IsString(item) && strlen(item->valuestring) < len;
    if (ok) {
        strcpy(name, item->valuestring);
    }
    cJSON_Delete(json);
    return ok;
}

static void cmd_playset(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    playset_t *playset = psram_calloc(1, sizeof(playset_t));
    if (playset == NULL) {
        p3a_test_reply_err("no memory");
        return;
    }
    char name[80];
    char why[120] = "";
    const bool save = strncmp(args, "save ", 5) == 0, active_save = strncmp(args, "active-save ", 12) == 0;
    if (save || active_save) {
        cJSON *json = cJSON_Parse(args + (save ? 5 : 12));
        esp_err_t err = json ? playset_from_json(json, playset, why, sizeof(why)) : ESP_ERR_INVALID_ARG;
        cJSON_Delete(json);
        if (err == ESP_OK) {
            err = save ? playset_store_save(playset) : playset_store_save_active(playset);
        }
        reply_result(err, why);
    } else if (strncmp(args, "load ", 5) == 0 && name_arg(args + 5, name, sizeof(name))) {
        reply_playset(playset_store_load(name, playset), playset);
    } else if (strcmp(args, "active-load") == 0) {
        reply_playset(playset_store_load_active(playset), playset);
    } else if (strcmp(args, "active-forget") == 0) {
        // The active playset's file sits beside the saved ones.
        char path[220];
        char *slash = playset_store_path("x", path, sizeof(path) - 24) == ESP_OK ? strrchr(path, '/') : NULL;
        if (slash == NULL) {
            p3a_test_reply_err("no playsets folder");
        } else {
            strcpy(slash + 1, "active.p3af");
            unlink(path);
            strcat(path, ".bak");
            unlink(path);
            reply_result(ESP_OK, NULL);
        }
    } else if (strncmp(args, "delete ", 7) == 0 && name_arg(args + 7, name, sizeof(name))) {
        reply_result(playset_store_delete(name), NULL);
    } else if (strncmp(args, "damage ", 7) == 0 && name_arg(args + 7, name, sizeof(name))) {
        char path[200];
        FILE *f = playset_store_path(name, path, sizeof(path)) == ESP_OK ? fopen(path, "r+b") : NULL;
        if (f == NULL) {
            p3a_test_reply_err("no such file");
        } else {
            uint8_t byte = 0;
            fseek(f, 32 + 300, SEEK_SET);  // inside the first channel's record
            fread(&byte, 1, 1, f);
            byte ^= 0x20;
            fseek(f, 32 + 300, SEEK_SET);
            fwrite(&byte, 1, 1, f);
            fclose(f);
            reply_result(ESP_OK, NULL);
        }
    } else if (strcmp(args, "list") == 0) {
        static playset_store_entry_t entries[PLAYSET_STORE_MAX];
        uint32_t count = 0, unreadable = 0;
        const esp_err_t err = playset_store_list(entries, PLAYSET_STORE_MAX, &count, &unreadable);
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "result", err == ESP_OK ? "ok" : p3a_err_to_name(err));
        cJSON_AddNumberToObject(out, "unreadable", unreadable);
        cJSON *names = cJSON_AddObjectToObject(out, "playsets");
        for (uint32_t i = 0; i < count; i++) {
            cJSON_AddNumberToObject(names, entries[i].name, entries[i].channel_count);
        }
        char *text = cJSON_PrintUnformatted(out);
        p3a_test_reply_ok(text ? text : "{}");
        cJSON_free(text);
        cJSON_Delete(out);
    } else {
        p3a_test_reply_err("playset save|load|delete|list|active-save|active-load|active-forget|damage");
    }
    free(playset);
}

const p3a_test_command_t p3a_test_playset_commands[] = {
    {"playset", cmd_playset},
};
const size_t p3a_test_playset_command_count = sizeof(p3a_test_playset_commands) / sizeof(p3a_test_playset_commands[0]);
