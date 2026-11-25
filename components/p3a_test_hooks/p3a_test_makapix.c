// SPDX-License-Identifier: Apache-2.0
// Test commands for Makapix Club.
//
// !makapix command {...}    hands the source one command as the link would: the whole message,
//                           {"command_id", "command_type", "payload"}. It is run within a second,
//                           on the Makapix task; the test watches for what it did.

#include <string.h>

#include "p3a_err.h"
#include "p3a_test_internal.h"
#include "source_makapix.h"

static void cmd_makapix(const char *args)
{
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "command ", 8) == 0) {
        const char *json = args + 8;
        const esp_err_t err = source_makapix_deliver(json, strlen(json));
        if (err == ESP_OK) {
            p3a_test_reply_ok("{\"result\":\"ok\"}");
        } else {
            p3a_test_reply_err(p3a_err_to_name(err));
        }
    } else {
        p3a_test_reply_err("makapix command <json>");
    }
}

const p3a_test_command_t p3a_test_makapix_commands[] = {
    {"makapix", cmd_makapix},
};
const size_t p3a_test_makapix_command_count = sizeof(p3a_test_makapix_commands) / sizeof(p3a_test_makapix_commands[0]);
