// SPDX-License-Identifier: Apache-2.0
// Test commands for the UI: put screens and notices up and take them down,
// through the same calls every layer uses (p3a_ui_hooks.h). Arguments are
// JSON.
//
// !ui screen {"screen": n, "up": true, "title": "...", "text": "...", "hint": "...", "progress": 40}
// !ui notice {"notice": n, "up": true, "text": "...", "ttl_ms": 0, "max_per_boot": 0}
//
// !ui touch [[fingers, x0, y0, x1, y1, hold_ms], ...]   fingers for the touch task, in
//                                      the panel's coordinates; answers at once
// !ui touched                          whether that script still runs, and the last
//                                      gesture recognised and what it did
// !ui usb mounted|unmounted|suspended|resumed    what the USB bus would report, as if a
//                                      computer were there (it cannot then read the
//                                      card, but the device behaves as if it could)
// !ui usb                              whether the card is handed over
//
// The numbers are p3a_screen_t and p3a_notice_t. What is then on the screen
// is read from GET /api/v2/ui.

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "p3a_err.h"
#include "p3a_test_internal.h"
#include "p3a_ui.h"
#include "p3a_ui_hooks.h"
#include "p3a_usb.h"

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

static void cmd_touch(const char *json_text)
{
    cJSON *json = cJSON_Parse(json_text);
    p3a_ui_touch_step_t steps[24];
    size_t count = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, json)
    {
        if (count == 24 || cJSON_GetArraySize(row) != 6) {
            count = 0;
            break;
        }
        p3a_ui_touch_step_t *s = &steps[count++];
        s->fingers = (uint8_t)cJSON_GetArrayItem(row, 0)->valueint;
        s->x[0] = cJSON_GetArrayItem(row, 1)->valueint;
        s->y[0] = cJSON_GetArrayItem(row, 2)->valueint;
        s->x[1] = cJSON_GetArrayItem(row, 3)->valueint;
        s->y[1] = cJSON_GetArrayItem(row, 4)->valueint;
        s->hold_ms = (uint32_t)cJSON_GetArrayItem(row, 5)->valueint;
    }
    cJSON_Delete(json);
    const esp_err_t err = count ? p3a_ui_touch_script(steps, count) : ESP_ERR_INVALID_ARG;
    char reply[64];
    snprintf(reply, sizeof(reply), "{\"result\":\"%s\"}", err == ESP_OK ? "ok" : p3a_err_to_name(err));
    p3a_test_reply_ok(reply);
}

static void cmd_touched(void)
{
    p3a_ui_touch_status_t st;
    p3a_ui_touch_get_status(&st);
    char reply[160];
    snprintf(reply, sizeof(reply), "{\"scripted\":%s,\"gestures\":%u,\"last_gesture\":\"%s\",\"last_action\":\"%s\"}",
             st.scripted ? "true" : "false", (unsigned)st.gestures, st.last_gesture ? st.last_gesture : "none",
             st.last_action ? st.last_action : "none");
    p3a_test_reply_ok(reply);
}

static void cmd_ui(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "touch ", 6) == 0) {
        cmd_touch(args + 6);
        return;
    }
    if (strcmp(args, "touched") == 0) {
        cmd_touched();
        return;
    }
    if (strncmp(args, "usb", 3) == 0) {
        static const char *const events[] = {"mounted", "unmounted", "suspended", "resumed"};
        for (int i = 0; i < 4; i++) {
            if (strcmp(args + 3, " ") != 0 && args[3] == ' ' && strcmp(args + 4, events[i]) == 0) {
                p3a_usb_session_event((p3a_usb_event_t)i);
            }
        }
        p3a_test_reply_ok(p3a_usb_is_sharing() ? "{\"sharing\":true}" : "{\"sharing\":false}");
        return;
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
    snprintf(words.hint, sizeof(words.hint), "%s", string(json, "hint"));
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
