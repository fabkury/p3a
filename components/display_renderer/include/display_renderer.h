// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// The display pipeline: who draws each frame, and when it reaches the panel.
//
// Two tasks on the render core. The producer takes a free framebuffer, asks
// the active source to draw a frame into it, lets the overlays draw on top,
// and queues it with the time it should stay on screen. The consumer paces
// queued frames to a virtual playhead, locks each to the nearest vsync edge
// and hands it to the panel. Three framebuffers let the producer keep one
// frame ahead, which hides a frame that was slow to make. The design, its
// timing rules and the reasons for them are in
// docs/infrastructure/display-pipeline.md and docs/jitter/REPORT.md; the
// timing code moved from 1.x unchanged and must stay that way unless the
// jitter pass bar is re-measured.
//
// What draws is not this component's business:
// - A SOURCE draws whole frames. There is a slot for artwork, one for the
//   user interface and one for test patterns, and one of them is active.
// - An OVERLAY draws on top of a finished frame (the FPS counter is one).
// Sources and overlays are registered by the code that owns them; this
// component depends on none of them.
//
// Rotation, "ignore frame delays", the FPS counter and the background colour
// follow their settings by themselves.
#ifndef DISPLAY_RENDERER_H
#define DISPLAY_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_ROTATION_0 = 0,
    DISPLAY_ROTATION_90 = 90,
    DISPLAY_ROTATION_180 = 180,
    DISPLAY_ROTATION_270 = 270
} display_rotation_t;

typedef enum {
    DISPLAY_SOURCE_ARTWORK,  // the player
    DISPLAY_SOURCE_UI,       // screens that take the whole panel
    DISPLAY_SOURCE_TEST,     // test patterns (test builds only); no overlay draws over it
    DISPLAY_SOURCE_COUNT_
} display_source_t;

// A source's answer when what is on the panel is still right: nothing is
// queued, the panel keeps showing the last frame, and the source is asked
// again in 50 ms. A static screen answers this instead of redrawing.
#define DISPLAY_FRAME_UNCHANGED (-2)

// Draws one frame into `buffer` (the whole framebuffer: P3A_DISPLAY_HEIGHT
// rows of display_renderer_row_stride() bytes, three bytes per pixel in the
// panel's order: blue, green, red). Called on the producer task.
//
// `must_redraw` is true when the previous frame on the panel is not this
// source's own last frame (the source just became active, or the panel was
// blank): DISPLAY_FRAME_UNCHANGED is then not an acceptable answer.
//
// Returns how long the frame should stay on screen in milliseconds,
// DISPLAY_FRAME_UNCHANGED, or another negative number to skip this frame
// after a failure (the panel keeps the last one).
typedef int (*display_frame_source_fn)(uint8_t *buffer, bool must_redraw, void *ctx);

// Draws on top of a finished frame. Called on the producer task.
typedef void (*display_overlay_fn)(uint8_t *buffer, void *ctx);

// Sets the pipeline up on the board's panel and framebuffers and starts its
// tasks. Call once, after p3a_board_display_init() and p3a_settings_start().
// Until a source is registered the panel is black.
esp_err_t display_renderer_start(void);

// Registers the function that draws for a slot; NULL removes it.
void display_renderer_set_source(display_source_t slot, display_frame_source_fn fn, void *ctx);

// Makes a slot the active one and waits (up to half a second) for the
// producer to follow. Frames already queued from the other source are
// dropped, so the change shows at the next vsync.
esp_err_t display_renderer_select_source(display_source_t slot);
display_source_t display_renderer_active_source(void);

// Adds an overlay. `over_ui` says whether it also draws when the UI source is
// active. ESP_ERR_NO_MEM when the few slots are taken.
esp_err_t display_renderer_add_overlay(display_overlay_fn fn, void *ctx, bool over_ui);

// Tells the consumer that what follows does not continue what came before
// (a new artwork, say): frames still queued are dropped and the next one is
// shown at once instead of on the old schedule.
void display_renderer_note_content_discontinuity(void);

// Black frames instead of the source's, for as long as it is set: pause.
// Brightness zero has the same effect by itself.
void display_renderer_set_blank(bool blank);

display_rotation_t display_renderer_get_rotation(void);
void display_renderer_get_background(uint8_t *r, uint8_t *g, uint8_t *b);
size_t display_renderer_row_stride(void);
size_t display_renderer_buffer_bytes(void);

// The framebuffer the panel is scanning out now, or NULL before the first
// frame. It stays the displayed one only until the next frame is presented:
// for test hooks reading a static screen, not for drawing.
const uint8_t *display_renderer_displayed_buffer(void);

// Scales an RGB888 image onto a framebuffer with nearest-neighbour sampling,
// on both cores. lookup_x and lookup_y map each destination column and row
// inside the scaled area to a source column and row; the area sits at
// (offset_x, offset_y) and, if has_borders, the rest of the frame is filled
// with the background colour. Blocks until done.
void display_renderer_parallel_upscale_rgb(const uint8_t *src_rgb, int src_w, int src_h, uint8_t *dst_buffer,
                                           const uint16_t *lookup_x, const uint16_t *lookup_y, int offset_x,
                                           int offset_y, int scaled_w, int scaled_h, bool has_borders,
                                           display_rotation_t rotation);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_RENDERER_H
