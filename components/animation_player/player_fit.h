// SPDX-License-Identifier: Apache-2.0
// Where a picture goes on the panel, and which file format a file really is.
// Pure: no IDF includes, host-tested (tests/host/player/test_player_fit.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int scaled_w, scaled_h;  // the picture's size on the panel, in panel pixels
    int offset_x, offset_y;  // its top left corner; the rest is border
    bool has_borders;
    // How many entries of each lookup table the upscaler reads. lookup_x maps
    // to source columns and lookup_y to source rows, always; turned a quarter,
    // the upscaler walks lookup_x down the panel and lookup_y across it.
    int lookup_x_len, lookup_y_len;
} player_fit_t;

// Fits a src_w by src_h picture to the panel: as large as it goes without
// cropping or changing its shape, centred. `quarter_turn` is true for
// rotations of 90 and 270 degrees. Sizes must be positive.
void player_fit_compute(int src_w, int src_h, int panel_w, int panel_h, bool quarter_turn, player_fit_t *out);

// Fills a nearest-neighbour lookup table: entry i of the first `used_len`
// takes source index i * src_len / used_len. The rest of the table, up to
// `table_len`, repeats the last entry, so that a reader that runs past the
// used part still reads inside the picture.
void player_fit_fill_lookup(uint16_t *table, int table_len, int used_len, int src_len);

// The formats the player opens. The numbers are the player's own; it maps
// them to decoder types.
typedef enum {
    PLAYER_FORMAT_UNKNOWN = 0,
    PLAYER_FORMAT_WEBP,
    PLAYER_FORMAT_GIF,
    PLAYER_FORMAT_PNG,  // also APNG
    PLAYER_FORMAT_JPEG,
    PLAYER_FORMAT_BMP,
} player_format_t;

// What a file is, by its first bytes. The file's name is not consulted: a
// download saved under the wrong extension still plays.
player_format_t player_format_sniff(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif
