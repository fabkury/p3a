// SPDX-License-Identifier: Apache-2.0
// A clip: one file opened for playing, with everything it needs to put its
// frames on the panel. Private to the player.
//
// A clip is used by one task at a time. The player's own task loads it and
// decodes its first frame; from the moment it is handed to the display
// pipeline until it is retired, only the pipeline's producer task touches it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "animation_decoder.h"
#include "esp_err.h"
#include "player_fit.h"

typedef struct {
    bool loaded;
    animation_decoder_t *decoder;
    animation_decoder_info_t info;
    uint8_t *file_data;  // NULL once the decoder no longer needs the file
    size_t file_size;
    bool smooth;         // the hardware (bilinear) scaler, else nearest neighbour

    // Decoded frames at the picture's own size, RGB888. Animations decode into
    // the two in turn; a still has only the first.
    uint8_t *frame[2];
    uint8_t next_frame;         // which of the two the next decode goes to
    const uint8_t *last_good;   // the most recent frame that decoded

    player_fit_t fit;
    int fit_rotation;           // the rotation `fit` and the tables were built for
    uint16_t *lookup_x, *lookup_y;

    bool first_pending;         // frame[0] holds the first frame, not yet shown
    uint32_t first_delay_ms;
    uint32_t background_seen;   // the background generation the last drawn frame used
    bool decode_failed;         // latched: the picture stays on its last good frame

    uint32_t frames_shown;      // frames of the artwork drawn since the clip was loaded
    uint32_t draws;             // times anything was drawn, redraws of the same frame included
} player_clip_t;

// Reads the file, opens its decoder and decodes the first frame. `clip`
// must be unloaded. On failure nothing is left allocated and the error says
// why in terms a caller can act on:
//   P3A_ERR_MISSING       there is no such file
//   P3A_ERR_NOT_PLAYABLE  not a format the player opens, or larger than it takes
//   P3A_ERR_CORRUPT       the bytes are damaged
//   ESP_ERR_NO_MEM        it does not fit in memory now
//   ESP_FAIL              the card could not be read
esp_err_t player_clip_load(player_clip_t *clip, const char *path, bool smooth);

// Frees everything. Safe on a clip that is not loaded.
void player_clip_unload(player_clip_t *clip);

// Draws the clip's next frame into a panel framebuffer. Returns how long the
// frame is to stay up in milliseconds, or DISPLAY_FRAME_UNCHANGED when the
// panel already shows it (a still that needs no redraw). A clip that is not
// loaded draws black. Called on the display pipeline's producer task.
int player_clip_render(player_clip_t *clip, uint8_t *dest, bool must_redraw);
