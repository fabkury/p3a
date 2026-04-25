// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#include "p3a_current_post.h"
#include <string.h>

// No lock: preserves the behavior of the previous implementation in makapix.c.
// Writes happen once per artwork swap (order of seconds apart) from a single
// call site in animation_player_render.c. Individual aligned 32-bit loads and
// stores are atomic on the ESP32-P4 RISC-V core, so neither field can tear.
// The (id, source, giphy_id) tuple is not updated atomically, but the race
// window is microseconds and consumers re-check source before acting on id.
//
// The giphy_id buffer is read by the touch router on swipe-up. A torn read
// would at worst yield an unrecognized Giphy ID, which the API would 404 on
// and surface as the error icon — acceptable failure mode for a sub-ms race.
static int32_t s_current_post_id = 0;
static int     s_current_post_source = 0;  // 0 == POST_SOURCE_NONE
static char    s_current_giphy_id[24] = "";

void p3a_current_post_set(int32_t post_id, int source, const char *giphy_id)
{
    s_current_post_id = post_id;
    s_current_post_source = source;
    if (giphy_id && giphy_id[0]) {
        // Copy then null-pad so a torn read can never see uninitialized bytes.
        size_t n = strlen(giphy_id);
        if (n >= sizeof(s_current_giphy_id)) n = sizeof(s_current_giphy_id) - 1;
        memcpy(s_current_giphy_id, giphy_id, n);
        memset(s_current_giphy_id + n, 0, sizeof(s_current_giphy_id) - n);
    } else {
        memset(s_current_giphy_id, 0, sizeof(s_current_giphy_id));
    }
}

void p3a_current_post_clear(void)
{
    s_current_post_id = 0;
    s_current_post_source = 0;
    memset(s_current_giphy_id, 0, sizeof(s_current_giphy_id));
}

int32_t p3a_current_post_get_id(void)
{
    return s_current_post_id;
}

int p3a_current_post_get_source(void)
{
    return s_current_post_source;
}

void p3a_current_post_get_giphy_id(char *out, size_t max_len)
{
    if (!out || max_len == 0) return;
    // strlcpy-equivalent on a buffer that is always null-terminated by writer.
    size_t n = strnlen(s_current_giphy_id, sizeof(s_current_giphy_id));
    if (n >= max_len) n = max_len - 1;
    memcpy(out, s_current_giphy_id, n);
    out[n] = '\0';
}
