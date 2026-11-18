// SPDX-License-Identifier: Apache-2.0
// Test console commands for the settings store and the recovery counters.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_recovery.h"
#include "p3a_settings.h"
#include "p3a_test_internal.h"

static atomic_int s_events;
static atomic_int s_last_id = -1;
static bool s_subscribed;

static void on_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    atomic_store(&s_last_id, event->payload.i32);
    atomic_fetch_add(&s_events, 1);
}

void p3a_test_settings_subscribe(void)
{
    if (!s_subscribed) {
        s_subscribed = p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_changed, NULL) == ESP_OK;
    }
}

static void reply_error(esp_err_t err, const char *why)
{
    char message[160];
    snprintf(message, sizeof(message), "%s: %s", p3a_err_to_name(err), why != NULL ? why : "");
    p3a_test_reply_err(message);
}

// settings get: every setting, secrets masked.
static void cmd_get(void)
{
    cJSON *object = p3a_settings_json();
    char *text = object != NULL ? cJSON_PrintUnformatted(object) : NULL;
    cJSON_Delete(object);
    if (text == NULL) {
        p3a_test_reply_err("no memory");
        return;
    }
    p3a_test_reply_ok(text);
    cJSON_free(text);
}

static void reply_changed(uint64_t changed)
{
    char json[1024];
    size_t n = (size_t)snprintf(json, sizeof(json), "{\"changed\":[");
    bool first = true;
    for (int i = 0; i < P3A_SET_COUNT && n < sizeof(json) - 80; i++) {
        if (changed & (1ULL << i)) {
            n += (size_t)snprintf(json + n, sizeof(json) - n, "%s\"%s\"", first ? "" : ",", p3a_setting_defs[i].key);
            first = false;
        }
    }
    snprintf(json + n, sizeof(json) - n, "],\"reboot_needed\":%s}", p3a_settings_reboot_needed() ? "true" : "false");
    p3a_test_reply_ok(json);
}

// settings set <json object>
static void cmd_set(const char *json)
{
    cJSON *object = cJSON_Parse(json);
    if (object == NULL) {
        p3a_test_reply_err("not JSON");
        return;
    }
    uint64_t changed = 0;
    char why[96] = "";
    esp_err_t err = p3a_settings_set_json(object, &changed, why, sizeof(why));
    cJSON_Delete(object);
    if (err != ESP_OK) {
        reply_error(err, why);
        return;
    }
    reply_changed(changed);
}

// settings typed <key> bool|int|str <value>: the single-setting setters.
static void cmd_typed(const char *args)
{
    char key[40], kind[8], value[100] = "";
    if (sscanf(args, "%39s %7s %99[^\n]", key, kind, value) < 2) {
        p3a_test_reply_err("usage: settings typed <key> bool|int|str <value>");
        return;
    }
    int id = p3a_settings_find(key);
    esp_err_t err;
    if (strcmp(kind, "bool") == 0) {
        err = p3a_settings_set_bool((p3a_setting_id_t)id, strcmp(value, "true") == 0);
    } else if (strcmp(kind, "int") == 0) {
        err = p3a_settings_set_int((p3a_setting_id_t)id, (int32_t)strtol(value, NULL, 10));
    } else {
        err = p3a_settings_set_str((p3a_setting_id_t)id, value);
    }
    if (err != ESP_OK) {
        reply_error(err, key);
        return;
    }
    p3a_test_reply_ok("{}");
}

// settings info: how the store loaded, and the change events seen.
static void cmd_info(void)
{
    p3a_settings_stats_t stats = p3a_settings_stats();
    int last = atomic_load(&s_last_id);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"count\":%d,\"loaded\":%s,\"blob_unreadable\":%s,\"skipped\":%d,\"stored_bytes\":%u,"
             "\"struct_bytes\":%u,\"events\":%d,\"last_changed\":\"%s\",\"reboot_needed\":%s}",
             (int)P3A_SET_COUNT, stats.loaded ? "true" : "false", stats.blob_unreadable ? "true" : "false",
             stats.skipped, (unsigned)stats.stored_bytes, (unsigned)sizeof(p3a_settings_t), atomic_load(&s_events),
             last >= 0 && last < P3A_SET_COUNT ? p3a_setting_defs[last].key : "",
             p3a_settings_reboot_needed() ? "true" : "false");
    p3a_test_reply_ok(json);
}

// settings plant <text>: replaces the stored blob with arbitrary bytes, to
// test what the next boot makes of them.
static void cmd_plant(const char *text)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("p3a", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "settings", text, strlen(text));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        reply_error(err, "plant");
        return;
    }
    p3a_test_reply_ok("{}");
}

// settings aside: the size of the blob kept aside after an unreadable load.
static void cmd_aside(void)
{
    nvs_handle_t nvs;
    size_t len = 0;
    char json[48];
    if (nvs_open("p3a", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, "settings_bad", NULL, &len) != ESP_OK) {
            len = 0;
        }
        nvs_close(nvs);
    }
    snprintf(json, sizeof(json), "{\"bytes\":%u}", (unsigned)len);
    p3a_test_reply_ok(json);
}

static void cmd_settings(const char *args)
{
    p3a_test_settings_subscribe();
    if (strcmp(args, "get") == 0) {
        cmd_get();
    } else if (strncmp(args, "set ", 4) == 0) {
        cmd_set(args + 4);
    } else if (strncmp(args, "typed ", 6) == 0) {
        cmd_typed(args + 6);
    } else if (strcmp(args, "info") == 0) {
        cmd_info();
    } else if (strncmp(args, "plant ", 6) == 0) {
        cmd_plant(args + 6);
    } else if (strcmp(args, "aside") == 0) {
        cmd_aside();
    } else if (strcmp(args, "factory-reset") == 0) {
        esp_err_t err = p3a_settings_factory_reset();
        if (err != ESP_OK) {
            reply_error(err, "factory reset");
        } else {
            p3a_test_reply_ok("{}");
        }
    } else {
        p3a_test_reply_err("usage: settings get|set <json>|typed <key> <kind> <value>|info|plant <text>|aside|factory-reset");
    }
}

// recovery <touch|wifi|link>                    read the counters
// recovery <touch|wifi|link> ok|fail <max>      report an outcome, as boot code would
static void cmd_recovery(const char *args)
{
    char name[8] = "", outcome[8] = "";
    int max_reboots = 1;
    int n = sscanf(args, "%7s %7s %d", name, outcome, &max_reboots);
    int kind = -1;
    for (int k = 0; k < P3A_RECOVERY_KIND_COUNT_; k++) {
        if (strcmp(name, p3a_recovery_name((p3a_recovery_kind_t)k)) == 0) {
            kind = k;
        }
    }
    if (n < 1 || kind < 0) {
        p3a_test_reply_err("usage: recovery touch|wifi|link [ok|fail <max>]");
        return;
    }
    const char *action = "";
    if (n >= 2) {
        static const char *const names[] = {"continue", "reboot", "degraded"};
        action = names[p3a_recovery_report((p3a_recovery_kind_t)kind, strcmp(outcome, "ok") == 0,
                                           (uint16_t)max_reboots)];
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"action\":\"%s\",\"total\":%u,\"streak\":%u}", action,
             (unsigned)p3a_recovery_total((p3a_recovery_kind_t)kind),
             (unsigned)p3a_recovery_streak((p3a_recovery_kind_t)kind));
    p3a_test_reply_ok(json);
}

const p3a_test_command_t p3a_test_settings_commands[] = {
    {"settings", cmd_settings},
    {"recovery", cmd_recovery},
};
const size_t p3a_test_settings_command_count =
    sizeof(p3a_test_settings_commands) / sizeof(p3a_test_settings_commands[0]);
