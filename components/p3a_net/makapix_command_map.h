// SPDX-License-Identifier: Apache-2.0
// What a command from Makapix Club asks the device to do. Pure
// (cJSON, the record and the playset only): host-tested in
// tests/host/net/test_makapix_command_map.c.
//
// A command arrives as a type and a payload (source_makapix hands them
// over as they came). This turns the pair into one action that names the
// scheduler call, the setting or the job to run; net_makapix_commands.c
// runs it. The same calls serve the HTTP API: the two transports are one
// implementation (D54).
//
// The commands (the server's own source is the reference; the pages under
// docs/reference/ differ):
//
//   swap_next, swap_back
//   show_artwork    {post_id, storage_key, native_format, title, storage_shard, width, height}
//   swap_to         {channel: "sdcard", filename} | {post_id, storage_key, art_url}
//   show_url        {artwork_url}
//   play_channel    {channel_name: all | promoted | sdcard | by_user | reactions | hashtag | pinned,
//                    user_sqid | hashtag | slug}
//   play_playset    {playset_name, channels: [{type: named | user | hashtag | sdcard, name,
//                    identifier, display_name, weight}]}; 1.x called it execute_playset, with
//                    "name" to also save it. A playset in p3a's own form (channels with
//                    "source") is taken as it is
//   set_background_color {r, g, b}
//   set_paused {paused}, set_brightness {value}, set_rotation {value}: acknowledged
//   set_mirror: acknowledged as unsupported
//
// Anything else is ignored, as 1.x did: the server may know commands this
// firmware does not.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "p3a_record.h"
#include "playset.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAKAPIX_DO_NOTHING = 0,  // a command this firmware does not know
    MAKAPIX_DO_REFUSE,       // a known command whose payload will not do; `text` says why
    MAKAPIX_DO_UNSUPPORTED,  // known, and not something this device does
    MAKAPIX_DO_NEXT,
    MAKAPIX_DO_BACK,
    MAKAPIX_DO_PLAYSET,      // `playset`; `save` to keep it under its name too
    MAKAPIX_DO_SHOW,         // `record`: fetch it if need be, and show it
    MAKAPIX_DO_SHOW_FILE,    // `text`: a file of the user's own, by name
    MAKAPIX_DO_SHOW_URL,     // `text`: an address
    MAKAPIX_DO_BACKGROUND,   // `text`: "#RRGGBB"
    MAKAPIX_DO_PAUSE,        // `value`: 1 to pause, 0 to resume
    MAKAPIX_DO_BRIGHTNESS,   // `value`: 0 to 100
    MAKAPIX_DO_ROTATION,     // `value`: 0, 90, 180 or 270
} makapix_do_t;

typedef struct {
    makapix_do_t kind;
    bool wants_ack;     // the server waits to hear how it went (the set_* commands)
    int32_t value;
    bool save;
    p3a_record_t record;
    char text[1024];
    playset_t *playset;  // allocated for MAKAPIX_DO_PLAYSET; makapix_action_free() releases it
} makapix_action_t;

// Never fails: what cannot be understood becomes MAKAPIX_DO_REFUSE or
// MAKAPIX_DO_NOTHING. `payload` may be NULL.
void makapix_command_map(const char *type, const cJSON *payload, makapix_action_t *out);
void makapix_action_free(makapix_action_t *action);

const char *makapix_do_name(makapix_do_t kind);

#ifdef __cplusplus
}
#endif
