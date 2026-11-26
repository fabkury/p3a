// SPDX-License-Identifier: Apache-2.0
// Test commands for the firmware update.
//
// !update source {"list": "...", "prefix": "..."}
//                           lists releases at `list` and takes images only from addresses that
//                           start with `prefix`: the test rig's mirror on the LAN. What was
//                           known of releases is forgotten.
// !update source {}         back to GitHub.

#include <string.h>

#include "cJSON.h"
#include "p3a_test_internal.h"
#include "p3a_update.h"

static void cmd_update(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "source", 6) != 0) {
        p3a_test_reply_err("update source {\"list\":..., \"prefix\":...}");
        return;
    }
    cJSON *object = cJSON_Parse(args + 6);
    if (!cJSON_IsObject(object)) {
        cJSON_Delete(object);
        p3a_test_reply_err("a JSON object is needed");
        return;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(object, "list");
    const cJSON *prefix = cJSON_GetObjectItemCaseSensitive(object, "prefix");
    p3a_update_set_source(cJSON_IsString(list) ? list->valuestring : NULL,
                          cJSON_IsString(prefix) ? prefix->valuestring : NULL);
    cJSON_Delete(object);
    p3a_test_reply_ok("{\"result\":\"ok\"}");
}

const p3a_test_command_t p3a_test_update_commands[] = {
    {"update", cmd_update},
};
const size_t p3a_test_update_command_count = sizeof(p3a_test_update_commands) / sizeof(p3a_test_update_commands[0]);
