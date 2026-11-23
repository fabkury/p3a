// SPDX-License-Identifier: Apache-2.0
#include "player_fit.h"

#include <string.h>

void player_fit_compute(int src_w, int src_h, int panel_w, int panel_h, bool quarter_turn, player_fit_t *out)
{
    // Turned a quarter, the picture's width runs down the panel.
    const int64_t w = quarter_turn ? src_h : src_w;
    const int64_t h = quarter_turn ? src_w : src_h;

    int scaled_w = panel_w;
    int scaled_h = panel_h;
    if (w * panel_h >= h * panel_w) {
        scaled_h = (int)(panel_w * h / w);  // wider than the panel: fit the width
    } else {
        scaled_w = (int)(panel_h * w / h);  // taller: fit the height
    }
    if (scaled_w < 1) {
        scaled_w = 1;
    }
    if (scaled_h < 1) {
        scaled_h = 1;
    }

    out->scaled_w = scaled_w;
    out->scaled_h = scaled_h;
    out->offset_x = (panel_w - scaled_w) / 2;
    out->offset_y = (panel_h - scaled_h) / 2;
    out->has_borders = scaled_w < panel_w || scaled_h < panel_h;
    out->lookup_x_len = quarter_turn ? scaled_h : scaled_w;
    out->lookup_y_len = quarter_turn ? scaled_w : scaled_h;
}

void player_fit_fill_lookup(uint16_t *table, int table_len, int used_len, int src_len)
{
    if (table_len <= 0 || src_len <= 0) {
        return;
    }
    if (used_len > table_len) {
        used_len = table_len;
    }
    if (used_len < 1) {
        used_len = 1;
    }
    for (int i = 0; i < used_len; i++) {
        int64_t at = (int64_t)i * src_len / used_len;
        if (at >= src_len) {
            at = src_len - 1;
        }
        table[i] = (uint16_t)at;
    }
    for (int i = used_len; i < table_len; i++) {
        table[i] = table[used_len - 1];
    }
}

// The public form answers in p3a_format_t numbers, which are these.
_Static_assert(PLAYER_FORMAT_WEBP == 1 && PLAYER_FORMAT_GIF == 2 && PLAYER_FORMAT_PNG == 3 &&
                   PLAYER_FORMAT_JPEG == 4 && PLAYER_FORMAT_BMP == 5,
               "player formats are numbered like p3a_format_t");

int animation_player_format_of(const uint8_t *data, size_t size)
{
    return (int)player_format_sniff(data, size);
}

player_format_t player_format_sniff(const uint8_t *data, size_t size)
{
    if (data == NULL) {
        return PLAYER_FORMAT_UNKNOWN;
    }
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        return PLAYER_FORMAT_WEBP;
    }
    if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) {
        return PLAYER_FORMAT_GIF;
    }
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size >= 8 && memcmp(data, png, 8) == 0) {
        return PLAYER_FORMAT_PNG;
    }
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return PLAYER_FORMAT_JPEG;
    }
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        return PLAYER_FORMAT_BMP;
    }
    return PLAYER_FORMAT_UNKNOWN;
}
