// SPDX-License-Identifier: Apache-2.0
// The UI on the panel: the model drawn as the display pipeline's UI source
// (a screen that covers the artwork) and as an overlay (a notice as a band
// over a playing artwork). See ui_internal.h.
//
// Both draw from a snapshot of the model, on the pipeline's producer task,
// and both are cheap when nothing changed: the source answers "unchanged"
// and the overlay returns at once when no banner is up.

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "display_renderer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "p3a_clock.h"
#include "p3a_board.h"
#include "p3a_event.h"
#include "p3a_settings.h"
#include "p3a_job.h"
#include "p3a_ui.h"
#include "animation_player.h"
#include "ui_clock.h"
#include "ui_credit.h"
#include "ui_draw.h"
#include "ui_internal.h"

static const char *TAG = "ui";

extern const uint8_t font_start[] asm("_binary_ui_p3afont_start");
extern const uint8_t font_end[] asm("_binary_ui_p3afont_end");

// The icons, in the order of p3a_icon_t (index 0, "none", has no picture).
#define ICON_FILE(name)                                                      \
    extern const uint8_t name##_start[] asm("_binary_" #name "_p3aimg_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_p3aimg_end");
ICON_FILE(like)
ICON_FILE(unlike)
ICON_FILE(click)
ICON_FILE(pin)
ICON_FILE(unpin)
ICON_FILE(error)
static ui_image_t s_icons[P3A_ICON_COUNT];

#define ICON_MS 4000

static ui_font_t s_font;
static atomic_bool s_banner_up;
static atomic_int s_indicator;       // 0 off, 1 busy, 2 failed
static atomic_int s_indicator_size;  // pixels; 0 when the setting turns it off
// The credit under the artwork: a source's mark, a picture file that lives
// for good. The two ends are stored one after the other; a pair caught
// half changed has the wrong length for its picture and is not drawn.
static _Atomic(const uint8_t *) s_credit_mark, s_credit_mark_end;
static atomic_uint s_ground;  // the background colour, which the band has: 0xRRGGBB
static _Atomic uint32_t s_credit_token, s_credit_w, s_credit_h;
static atomic_bool s_credit_on;
// An icon per corner and when it goes, in milliseconds since boot.
static atomic_int s_icon[P3A_ICON_PLACES];
static _Atomic int64_t s_icon_until_ms[P3A_ICON_PLACES];
// The clock's settings, read on the producer task.
static atomic_bool s_clock_on, s_clock_24h;
static atomic_int s_clock_place, s_clock_scale, s_clock_outline;
static _Atomic uint32_t s_clock_colour, s_clock_outline_colour;
static atomic_int s_clock_minute = -1;  // the minute last drawn, to know when to draw again
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

// The overlay over the artwork: the processing indicator, and a notice's band.
static void draw_banner(uint8_t *buffer, void *ctx)
{
    (void)ctx;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    // First, so that everything else is seen over it: the source's mark in
    // the band under its artwork.
    const uint8_t *credit = atomic_load(&s_credit_mark), *credit_end = atomic_load(&s_credit_mark_end);
    ui_image_t mark;
    if (credit != NULL && credit_end > credit && atomic_load(&s_credit_on) &&
        ui_image_open(&mark, credit, (size_t)(credit_end - credit))) {
        // Only under the picture it was worked out for: after a change of
        // artwork the mark waits for the job that knows the new one.
        animation_player_status_t player;
        animation_player_get_status(&player);
        if (player.token == atomic_load(&s_credit_token)) {
            ui_canvas_t below;
            canvas_for(buffer, &below);
            const ui_credit_place_t place =
                ui_credit_place((int32_t)atomic_load(&s_credit_w), (int32_t)atomic_load(&s_credit_h),
                                ui_canvas_view_width(&below), ui_canvas_view_height(&below), mark.width, mark.height);
            if (place.show) {
                ui_image_draw_on(&below, &mark, place.x, place.y, atomic_load(&s_ground));
            }
        }
    }
    for (int place = 0; place < P3A_ICON_PLACES; place++) {
        const int icon = atomic_load(&s_icon[place]);
        if (icon > 0 && icon < P3A_ICON_COUNT && now_ms < atomic_load(&s_icon_until_ms[place])) {
            ui_canvas_t corner;
            canvas_for(buffer, &corner);
            ui_draw_icon(&corner, &s_icons[icon], place == P3A_ICON_TOP_LEFT);
        }
    }
    if (atomic_load(&s_clock_on) && p3a_clock_is_set()) {
        const time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        char text[UI_CLOCK_TEXT_LEN];
        ui_clock_text(local.tm_hour, local.tm_min, atomic_load(&s_clock_24h), text);
        const ui_clock_style_t clock = {
            .place = (ui_clock_place_t)atomic_load(&s_clock_place),
            .scale = atomic_load(&s_clock_scale),
            .colour = atomic_load(&s_clock_colour),
            .outline_colour = atomic_load(&s_clock_outline_colour),
            .outline = atomic_load(&s_clock_outline),
        };
        ui_canvas_t face;
        canvas_for(buffer, &face);
        ui_draw_clock(&face, &s_font, &clock, text);
        atomic_store(&s_clock_minute, local.tm_hour * 60 + local.tm_min);
    }
    if (atomic_load(&s_banner_up)) {
        p3a_ui_state_t state;
        p3a_ui_get_state(&state);
        if (state.banner && state.has_content) {
            ui_canvas_t canvas;
            canvas_for(buffer, &canvas);
            ui_style_t style;
            ui_style_default(&style, &s_font, ui_canvas_view_width(&canvas));
            ui_draw_banner(&canvas, &style, &state.content);
        }
    }
    // Last, so that it is seen over a notice's band: the band says why
    // something failed, the indicator that something was asked for.
    const int indicator = atomic_load(&s_indicator), size = atomic_load(&s_indicator_size);
    if (indicator != 0 && size > 0) {
        ui_canvas_t corner;
        canvas_for(buffer, &corner);
        ui_draw_indicator(&corner, size, indicator == 1 ? 0x2090FF : 0xFF3030);
    }
}

int ui_display_button_at(int32_t x, int32_t y)
{
    p3a_ui_state_t state;
    p3a_ui_get_state(&state);
    if (!state.has_content) {
        return -1;
    }
    ui_canvas_t canvas;
    canvas_for(NULL, &canvas);
    ui_style_t style;
    ui_style_default(&style, &s_font, ui_canvas_view_width(&canvas));
    return ui_draw_button_at(&style, ui_canvas_view_width(&canvas), ui_canvas_view_height(&canvas), &state.content, x, y);
}

bool p3a_ui_button_rect(int index, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    p3a_ui_state_t state;
    p3a_ui_get_state(&state);
    if (!state.has_content) {
        return false;
    }
    ui_canvas_t canvas;
    canvas_for(NULL, &canvas);
    ui_style_t style;
    ui_style_default(&style, &s_font, ui_canvas_view_width(&canvas));
    return ui_draw_button_rect(&style, ui_canvas_view_width(&canvas), ui_canvas_view_height(&canvas), &state.content,
                               index, x, y, w, h);
}

void ui_display_set_credit(const uint8_t *mark, const uint8_t *mark_end, uint32_t token, uint32_t width,
                           uint32_t height)
{
    // Both are stored, whichever changed: with `||` a new mark left the old
    // token in place, and the first GIF after another source's artwork went
    // without its credit until something else asked again.
    const bool new_mark = atomic_exchange(&s_credit_mark, mark) != mark;
    atomic_store(&s_credit_mark_end, mark_end);
    const bool new_token = atomic_exchange(&s_credit_token, token) != token;
    const bool changed = new_mark || new_token;
    atomic_store(&s_credit_w, width);
    atomic_store(&s_credit_h, height);
    if (changed) {
        display_renderer_request_redraw();
    }
}

void ui_display_set_indicator(int show)
{
    if (atomic_exchange(&s_indicator, show) != show) {
        display_renderer_request_redraw();
    }
}

static void take_settings(void)
{
    static p3a_settings_t settings;  // too large for a stack; guarded by being called from one task at a time
    p3a_settings_get(&settings);
    atomic_store(&s_indicator_size, settings.indicator_enabled ? (int)settings.indicator_size : 0);
    atomic_store(&s_credit_on, settings.source_credit);
    atomic_store(&s_ground, settings.background_color);
    atomic_store(&s_clock_on, settings.clock_enabled);
    atomic_store(&s_clock_24h, settings.clock_24h);
    atomic_store(&s_clock_place, (int)settings.clock_position);
    atomic_store(&s_clock_scale, (int)settings.clock_size);
    atomic_store(&s_clock_outline, (int)settings.clock_outline);
    atomic_store(&s_clock_colour, settings.clock_color);
    atomic_store(&s_clock_outline_colour, settings.clock_outline_color);
    // The device's local time is the clock's alone so far; the zone is set
    // here, for the whole program, from the one setting.
    setenv("TZ", settings.timezone[0] ? settings.timezone : "UTC0", 1);
    tzset();
}

void ui_display_icon(int icon, int place)
{
    if (place < 0 || place >= P3A_ICON_PLACES) {
        return;
    }
    atomic_store(&s_icon_until_ms[place], esp_timer_get_time() / 1000 + ICON_MS);
    atomic_store(&s_icon[place], icon);
    display_renderer_request_redraw();
}

void ui_display_tick(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    bool redraw = false;
    // An icon whose time is up comes off a still artwork too.
    for (int place = 0; place < P3A_ICON_PLACES; place++) {
        if (atomic_load(&s_icon[place]) != 0 && now_ms >= atomic_load(&s_icon_until_ms[place])) {
            atomic_store(&s_icon[place], 0);
            redraw = true;
        }
    }
    // The clock: when the minute turns, and when the time first becomes known.
    if (atomic_load(&s_clock_on) && p3a_clock_is_set()) {
        const time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        redraw |= local.tm_hour * 60 + local.tm_min != atomic_load(&s_clock_minute);
    }
    if (redraw) {
        display_renderer_request_redraw();
    }
}

static void on_setting_changed(const p3a_event_t *event, void *ctx)
{
    (void)ctx;
    switch (event->payload.i32) {
    case P3A_SET_INDICATOR_ENABLED:
    case P3A_SET_INDICATOR_SIZE:
    case P3A_SET_SOURCE_CREDIT:
    case P3A_SET_BACKGROUND_COLOR:
    case P3A_SET_CLOCK_ENABLED:
    case P3A_SET_CLOCK_24H:
    case P3A_SET_CLOCK_POSITION:
    case P3A_SET_CLOCK_SIZE:
    case P3A_SET_CLOCK_COLOR:
    case P3A_SET_CLOCK_OUTLINE_COLOR:
    case P3A_SET_CLOCK_OUTLINE:
    case P3A_SET_TIMEZONE:
        take_settings();
        atomic_store(&s_clock_minute, -1);
        display_renderer_request_redraw();
        break;
    default:
        break;
    }
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
    static const struct {
        const uint8_t *start, *end;
    } files[P3A_ICON_COUNT] = {
        [P3A_ICON_LIKE] = {like_start, like_end},       [P3A_ICON_UNLIKE] = {unlike_start, unlike_end},
        [P3A_ICON_CLICK] = {click_start, click_end},    [P3A_ICON_PIN] = {pin_start, pin_end},
        [P3A_ICON_UNPIN] = {unpin_start, unpin_end},    [P3A_ICON_ERROR] = {error_start, error_end},
    };
    for (int i = 1; i < P3A_ICON_COUNT; i++) {
        if (!ui_image_open(&s_icons[i], files[i].start, (size_t)(files[i].end - files[i].start))) {
            ESP_LOGE(TAG, "built-in icon %d is damaged", i);
        }
    }
    take_settings();
    p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting_changed, NULL);
    display_renderer_set_source(DISPLAY_SOURCE_UI, draw_screen, NULL);
    return display_renderer_add_overlay(draw_banner, NULL, false);
}
