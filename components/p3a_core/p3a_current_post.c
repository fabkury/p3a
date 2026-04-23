// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "p3a_current_post.h"

// No lock: preserves the behavior of the previous implementation in makapix.c.
// Writes happen once per artwork swap (order of seconds apart) from a single
// call site in animation_player_render.c. Individual aligned 32-bit loads and
// stores are atomic on the ESP32-P4 RISC-V core, so neither field can tear.
// The (id, source) *pair* is not updated atomically, but the race window is
// microseconds and consumers re-check source before acting on id.
static int32_t s_current_post_id = 0;
static int     s_current_post_source = 0;  // 0 == POST_SOURCE_NONE

void p3a_current_post_set(int32_t post_id, int source)
{
    s_current_post_id = post_id;
    s_current_post_source = source;
}

void p3a_current_post_clear(void)
{
    s_current_post_id = 0;
    s_current_post_source = 0;
}

int32_t p3a_current_post_get_id(void)
{
    return s_current_post_id;
}

int p3a_current_post_get_source(void)
{
    return s_current_post_source;
}
