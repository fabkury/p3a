// SPDX-License-Identifier: Apache-2.0
// Shared by the test-hook sources. Each area of the firmware gets one file of
// console commands (p3a_test_<area>.c) exporting a table that the console
// lists in p3a_test_console.c.
#pragma once

#include <stddef.h>

typedef void (*p3a_test_command_fn)(const char *args);

typedef struct {
    const char *name;
    p3a_test_command_fn fn;
} p3a_test_command_t;

// Exactly one of these per command, from the task that received it.
void p3a_test_reply_ok(const char *json);
void p3a_test_reply_err(const char *message);

// Command tables.
extern const p3a_test_command_t p3a_test_util_commands[];
extern const size_t p3a_test_util_command_count;
extern const p3a_test_command_t p3a_test_board_commands[];
extern const size_t p3a_test_board_command_count;
extern const p3a_test_command_t p3a_test_storage_commands[];
extern const size_t p3a_test_storage_command_count;
extern const p3a_test_command_t p3a_test_settings_commands[];
extern const size_t p3a_test_settings_command_count;

// Starts counting setting-change events. Called when the console starts, so
// that changes made before the first settings command are counted too.
void p3a_test_settings_subscribe(void);

extern const p3a_test_command_t p3a_test_net_commands[];
extern const size_t p3a_test_net_command_count;

// Starts counting network up and down events, from the start of the console.
void p3a_test_net_subscribe(void);

// The framebuffer the board test commands last presented, or NULL.
const unsigned char *p3a_test_shown_buffer(void);

// Adds the /api/v2/test/ routes and starts capturing log lines.
void p3a_test_http_start(void);

// Adds the decoder test route and its worker task.
void p3a_test_decode_start(void);

// Adds the player test routes and listens to the player.
void p3a_test_player_start(void);
