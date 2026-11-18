// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// One interface over every image format p3a plays: WebP, GIF, PNG and APNG,
// JPEG, BMP, animated or still.
//
// A decoder is opened on the bytes of a file held in memory and then asked
// for one frame after another. Frames come out as RGB888, 3 bytes per pixel,
// canvas_width by canvas_height, with any transparency already blended onto
// the background colour (animation_decoder_set_background). A still image is
// an animation of one frame.
//
// The bytes are untrusted: files come from the internet and from users. A
// decoder must refuse a malformed file with an error, never read or write
// outside its buffers, and never trust a size it has not checked.
#ifndef ANIMATION_DECODER_H
#define ANIMATION_DECODER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct animation_decoder_s animation_decoder_t;

typedef enum {
    ANIMATION_DECODER_TYPE_WEBP,
    ANIMATION_DECODER_TYPE_GIF,
    ANIMATION_DECODER_TYPE_PNG,
    ANIMATION_DECODER_TYPE_JPEG,
    ANIMATION_DECODER_TYPE_BMP,
    ANIMATION_DECODER_TYPE_COUNT_
} animation_decoder_type_t;

// How long a still image's one frame is shown before it is asked for again.
#define ANIMATION_DECODER_STILL_FRAME_DELAY_MS 100

// The largest canvas a decoder accepts, per side. Larger files are refused
// before anything is allocated. (The software JPEG path downscales while it
// decodes and applies this limit to the result.)
#define ANIMATION_DECODER_MAX_DIMENSION 4096

typedef struct {
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    bool has_transparency;
    // True when init decoded the whole file and the decoder no longer reads
    // the source bytes (JPEG, PNG, still WebP, BMP). With frame_count <= 1
    // the caller may then free its copy of the file at once, which saves
    // 0.1 to 2 MiB of PSRAM per still image. False for formats that read the
    // file as they go (animated WebP, GIF, APNG).
    bool source_consumed;
} animation_decoder_info_t;

// Opens a decoder on `size` bytes at `data`. Unless source_consumed is
// reported, the bytes must stay valid until animation_decoder_unload().
esp_err_t animation_decoder_init(animation_decoder_t **decoder, animation_decoder_type_t type, const uint8_t *data,
                                 size_t size);

esp_err_t animation_decoder_get_info(animation_decoder_t *decoder, animation_decoder_info_t *info);

// Decodes the next frame into rgb_buffer, which must hold canvas_width *
// canvas_height * 3 bytes.
//
// What happens after the last frame differs by format, as it did in 1.x, and
// a caller handles both: GIF starts again from its first frame by itself;
// WebP and APNG report ESP_ERR_INVALID_STATE until animation_decoder_reset().
//
// A still image without transparency gives its frame once: the decoder then
// frees its copy, since the caller holds the pixels, and further calls
// report ESP_ERR_INVALID_STATE. A still with transparency can be asked for
// again, which is how it gets re-blended when the background colour changes.
esp_err_t animation_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);

// A frame delay this short is not believed. Many files, GIFs above all,
// declare 0 or 10 ms; every browser shows such a frame for 100 ms, so that is
// the speed their authors saw. p3a does the same (decision D37). 1.x played
// them as fast as the panel allowed.
#define ANIMATION_DECODER_SHORT_DELAY_MS    10
#define ANIMATION_DECODER_SHORT_DELAY_AS_MS 100

// The delay to use for a frame whose file declares `declared_ms`.
static inline uint32_t animation_decoder_effective_delay_ms(uint32_t declared_ms)
{
    return declared_ms <= ANIMATION_DECODER_SHORT_DELAY_MS ? ANIMATION_DECODER_SHORT_DELAY_AS_MS : declared_ms;
}

// How long the frame just decoded should stay on screen, with the rule above
// applied.
esp_err_t animation_decoder_get_frame_delay(animation_decoder_t *decoder, uint32_t *delay_ms);

// Back to the first frame.
esp_err_t animation_decoder_reset(animation_decoder_t *decoder);

// Releases the decoder and sets *decoder to NULL.
void animation_decoder_unload(animation_decoder_t **decoder);

// --- the background colour ------------------------------------------------------
//
// Transparent pixels are blended onto this colour as each frame is decoded.
// animation_decoder_start() takes it from the background_color setting and
// follows changes to it; a decoder with a still, transparent image picks a
// change up the next time its frame is asked for.

// Call once at boot, after p3a_settings_start() and p3a_event_bus_start().
esp_err_t animation_decoder_start(void);

void animation_decoder_set_background(uint8_t r, uint8_t g, uint8_t b);
void animation_decoder_get_background(uint8_t *r, uint8_t *g, uint8_t *b);
// Changes each time the colour does, so that a decoder holding composited
// pixels knows when to composite again.
uint32_t animation_decoder_background_generation(void);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_DECODER_H
