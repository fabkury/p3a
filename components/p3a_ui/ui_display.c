// SPDX-License-Identifier: Apache-2.0
// The UI on the panel: the model drawn as the display pipeline's UI source
// (a screen that covers the artwork) and as an overlay (a notice as a band
// over a playing artwork). See ui_internal.h.
//
// Both draw from a snapshot of the model, on the pipeline's producer task,
// and both are cheap when nothing changed: the source answers "unchanged"
// and the overlay returns at once when no banner is up.

#include <stdatomic.h>
#include <string.h>

#include "display_renderer.h"
#include "esp_log.h"
#include "p3a_board.h"
#include "p3a_job.h"
#include "p3a_ui.h"
#include "ui_draw.h"
#include "ui_internal.h"

static const char *TAG = "ui";

extern const uint8_t font_start[] asm("_binary_ui_p3afont_start");
extern const uint8_t font_end[] asm("_binary_ui_p3afont_end");

static ui_font_t s_font;
static atomic_bool s_banner_up;
static atomic_bool s_covers;         // what the model says
static atomic_bool s_select_queued;
// Touched on the producer task only.
static uint32_t s_drawn_revision;
static int s_drawn_rotation = -1;

static void canvas_for(uint8_t *buffer, ui_canvas_t *canvas)
{
    canvas->pixels = buffer;
    canvas->width = P3A_DISPLAY_WIDTH;
    canvas->height = P3A_DISPLAY_HEIGHT;
    canvas->stride = display_renderer_row_stride();
    canvas->rotation = (int)display_renderer_get_rotation();
}

static int draw_screen(uint8_t *buffer, bool must_redraw, void *ctx)
{
    (void)ctx;
    p3a_ui_state_t state;
    p3a_ui_get_state(&state);
    if (state.screen == P3A_SCREEN_INTRO) {
        const int frame_ms = ui_intro_render(buffer);
        if (frame_ms > 0) {
            s_drawn_rotation = -1;  // whatever is drawn after this is drawn afresh
            return frame_ms;
        }
        // Over: the screen comes down, and what was waiting under it shows.
        // Until the model has followed, the last frame stays.
        p3a_ui_screen_hide(P3A_SCREEN_INTRO);
        return must_redraw ? -1 : DISPLAY_FRAME_UNCHANGED;
    }
    ui_canvas_t canvas;
    canvas_for(buffer, &canvas);
    if (!must_redraw && state.revision == s_drawn_revision && canvas.rotation == s_drawn_rotation) {
        return DISPLAY_FRAME_UNCHANGED;
    }
    ui_style_t style;
    ui_style_default(&style, &s_font, ui_canvas_view_width(&canvas));
    if (state.screen == P3A_SCREEN_FATAL) {
        style.title = 0xFF5050;
    }
    ui_draw_screen(&canvas, &style, state.has_content ? &state.content : NULL);
    s_drawn_revision = state.revision;
    s_drawn_rotation = canvas.rotation;
    return 100;
}

static void draw_banner(uint8_t *buffer, void *ctx)
{
    (void)ctx;
    if (!atomic_load(&s_banner_up)) {
        return;
    }
    p3a_ui_state_t state;
    p3a_ui_get_state(&state);
    if (!state.banner || !state.has_content) {
        return;
    }
    ui_canvas_t canvas;
    canvas_for(buffer, &canvas);
    ui_style_t style;
    ui_style_default(&style, &s_font, ui_canvas_view_width(&canvas));
    ui_draw_banner(&canvas, &style, &state.content);
}

// A job: switching the pipeline's source waits for the producer, which the
// tasks that change the model must not.
static esp_err_t select_job(void *arg)
{
    (void)arg;
    atomic_store(&s_select_queued, false);
    const bool covers = atomic_load(&s_covers);
    const display_source_t want = covers ? DISPLAY_SOURCE_UI : DISPLAY_SOURCE_ARTWORK;
    const display_source_t active = display_renderer_active_source();
    // A test pattern stays until whoever put it up takes it down.
    if (active != want && active != DISPLAY_SOURCE_TEST) {
        return display_renderer_select_source(want);
    }
    return ESP_OK;
}

void ui_display_changed(bool covers_artwork, bool banner)
{
    atomic_store(&s_banner_up, banner);
    atomic_store(&s_covers, covers_artwork);
    display_renderer_request_redraw();
    if (!atomic_exchange(&s_select_queued, true) &&
        p3a_job_submit(P3A_JOBS_UI, "ui: screen", select_job, NULL, NULL) != ESP_OK) {
        atomic_store(&s_select_queued, false);
    }
}

esp_err_t ui_display_start(void)
{
    if (!ui_font_open(&s_font, font_start, (size_t)(font_end - font_start))) {
        ESP_LOGE(TAG, "the built-in font is damaged; screens will have no text");
    }
    display_renderer_set_source(DISPLAY_SOURCE_UI, draw_screen, NULL);
    return display_renderer_add_overlay(draw_banner, NULL, false);
}
