// SPDX-License-Identifier: Apache-2.0
// The animation player: turns one artwork file into frames on the panel.
//
// It plays what it is told to play and reports what became of the request.
// It does not choose artworks, time them, retry, or delete anything: that is
// the loader pipeline above it (M2.4). It is the artwork source of the
// display pipeline (display_renderer.h).
//
// A request goes through three moments, each reported once:
//
//   ready   the file is read, its decoder open, its first frame decoded.
//           The previous artwork is still on the panel.
//   shown   the display pipeline has drawn the new artwork's first frame.
//           Only now has the artwork been "played".
//   failed  the request ended without being shown: the file could not be
//           loaded, or a newer request replaced it first.
//
// "failed" may follow "ready" (replaced before it was shown); nothing
// follows "shown" or "failed". The previous artwork keeps playing until the
// new one is shown, and through any failure.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANIMATION_PLAYER_PATH_MAX 256

typedef struct {
    char path[ANIMATION_PLAYER_PATH_MAX];  // the file; its format is read from its bytes, not its name
    bool smooth;     // scale with the hardware (bilinear) scaler; false is nearest neighbour, for pixel art
    uint32_t token;  // the caller's name for this request, handed back in every report. Not 0
} animation_player_request_t;

// Reports. They are called on the player's own task, never on the display
// pipeline's; they may take their time, but the next request is not started
// until they return. One exception: a request replaced before the player's
// task had even picked it up is reported as failed from inside the
// animation_player_play() call that replaced it, on that caller's task.
typedef struct {
    void (*ready)(uint32_t token, void *ctx);
    void (*shown)(uint32_t token, void *ctx);
    // err: P3A_ERR_MISSING, P3A_ERR_NOT_PLAYABLE, P3A_ERR_CORRUPT, ESP_ERR_NO_MEM,
    // ESP_FAIL (the card could not be read), or ESP_ERR_NOT_FINISHED (a newer
    // request replaced this one; nothing is wrong with the file).
    void (*failed)(uint32_t token, esp_err_t err, void *ctx);
} animation_player_listener_t;

typedef struct {
    uint32_t token;         // what is on the panel; 0 when nothing is
    uint32_t width, height; // its own size in pixels
    uint32_t frame_count;   // 1 for a still
    uint32_t frames_shown;  // frames of it drawn since it was shown
    uint32_t draws;         // times it was drawn, redraws of the same frame included
    uint32_t loops;         // times an animation has played through
    bool decode_failed;     // it stopped decoding part way and is held on its last good frame
    bool loading;           // a request is being loaded or waits to be shown
} animation_player_status_t;

// Starts the player's task and registers it as the display pipeline's
// artwork source. Call once, after display_renderer_start().
esp_err_t animation_player_start(void);

// Who hears the reports. One listener; set it before the first request.
void animation_player_set_listener(const animation_player_listener_t *listener, void *ctx);

// Asks for a file to be played. Returns at once; the outcome arrives through
// the listener. A request made while another is still loading or waiting to
// be shown replaces it.
esp_err_t animation_player_play(const animation_player_request_t *request);

// Asks for nothing to be played: the panel goes black and the current
// artwork's memory is freed. Reported like any request, under `token`.
esp_err_t animation_player_stop(uint32_t token);

void animation_player_get_status(animation_player_status_t *out);

#ifdef __cplusplus
}
#endif
