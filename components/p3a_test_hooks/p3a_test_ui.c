// SPDX-License-Identifier: Apache-2.0
// Test commands for the UI: put screens and notices up and take them down,
// through the same calls every layer uses (p3a_ui_hooks.h). Arguments are
// JSON.
//
// !ui screen {"screen": n, "up": true, "title": "...", "text": "...", "progress": 40}
// !ui notice {"notice": n, "up": true, "text": "...", "ttl_ms": 0, "max_per_boot": 0}
//
// The numbers are p3a_screen_t and p3a_notice_t. What is then on the screen
// is read from GET /api/v2/ui.

#include <string.h>

#include "cJSON.h"
#include "p3a_test_internal.h"
#include "p3a_ui_hooks.h"

static int number(const cJSON *json, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *string(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static void cmd_ui(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    const bool screen = strncmp(args, "screen ", 7) == 0;
    const bool notice = strncmp(args, "notice ", 7) == 0;
    cJSON *json = (screen || notice) ? cJSON_Parse(args + 7) : NULL;
    if (json == NULL) {
        p3a_test_reply_err("ui screen {\"screen\":n,\"up\":bool,...} | ui notice {\"notice\":n,\"up\":bool,...}");
        return;
    }
    p3a_ui_content_t words;
    p3a_ui_content(&words, string(json, "title"), number(json, "progress", -1), "%s", string(json, "text"));
    const bool up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "up"));
    if (screen) {
        const p3a_screen_t which = (p3a_screen_t)number(json, "screen", -1);
        up ? p3a_ui_screen_show(which, &words) : p3a_ui_screen_hide(which);
    } else {
        const p3a_notice_t which = (p3a_notice_t)number(json, "notice", -1);
        up ? p3a_ui_notice_show(which, &words, (uint32_t)number(json, "ttl_ms", 0), (uint32_t)number(json, "max_per_boot", 0))
           : p3a_ui_notice_hide(which);
    }
    cJSON_Delete(json);
    p3a_test_reply_ok("{\"result\":\"ok\"}");
}

const p3a_test_command_t p3a_test_ui_commands[] = {
    {"ui", cmd_ui},
};
const size_t p3a_test_ui_command_count = sizeof(p3a_test_ui_commands) / sizeof(p3a_test_ui_commands[0]);
