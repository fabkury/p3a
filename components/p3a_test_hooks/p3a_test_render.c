// SPDX-License-Identifier: Apache-2.0
// Test hook for the display pipeline: put a picture on the panel the way the
// player will, through the artwork source and one of the two upscalers.
//
//   POST /api/v2/test/render?w=W&h=H[&mode=cpu|ppa]
//        body: W x H pixels of RGB888. The picture is scaled to fit the panel
//        (keeping its shape, centred, the rest filled with the background
//        colour), in the rotation the settings say, and stays up until the
//        next call. Answers where it was placed.
//   POST /api/v2/test/render/blank?on=1|0     black frames, as pause does
//   POST /api/v2/test/render/release          back to an empty artwork slot
//
// The picture is redrawn by itself when the rotation or the background
// colour changes, as the player's will be.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_ppa_upscaler.h"
#include "display_renderer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "p3a_http.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"

typedef struct {
    uint8_t *pixels;
    int w, h;
    bool use_ppa;
    // Where the last frame put it.
    int scaled_w, scaled_h, offset_x, offset_y;
    esp_err_t last_err;
} picture_t;

static SemaphoreHandle_t s_lock;
static picture_t s_picture;
static uint16_t *s_lookup_x;  // P3A_DISPLAY_WIDTH entries each
static uint16_t *s_lookup_y;
static atomic_bool s_fresh;       // a new picture was posted
static _Atomic(uint8_t *) s_drawn_into;
static int s_last_rotation = -1;
static uint32_t s_last_background;
static atomic_uint s_frames_drawn;

static uint32_t background_now(void)
{
    uint8_t r, g, b;
    display_renderer_get_background(&r, &g, &b);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// The artwork source: called on the producer task for every frame.
static int picture_source(uint8_t *buffer, bool must_redraw, void *ctx)
{
    (void)ctx;
    const int rotation = display_renderer_get_rotation();
    const uint32_t background = background_now();
    bool fresh = atomic_exchange(&s_fresh, false);
    if (!fresh && !must_redraw && rotation == s_last_rotation && background == s_last_background) {
        return DISPLAY_FRAME_UNCHANGED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    picture_t *p = &s_picture;
    if (p->pixels == NULL) {
        xSemaphoreGive(s_lock);
        memset(buffer, 0, display_renderer_buffer_bytes());
        return 100;
    }
    // Fit the picture to the panel. Turned a quarter, its width runs down the panel.
    const bool quarter = rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270;
    const int pic_w = quarter ? p->h : p->w;
    const int pic_h = quarter ? p->w : p->h;
    int scaled_w = P3A_DISPLAY_WIDTH;
    int scaled_h = (int)((int64_t)pic_h * P3A_DISPLAY_WIDTH / pic_w);
    if (scaled_h > P3A_DISPLAY_HEIGHT) {
        scaled_h = P3A_DISPLAY_HEIGHT;
        scaled_w = (int)((int64_t)pic_w * P3A_DISPLAY_HEIGHT / pic_h);
    }
    if (scaled_w < 1) {
        scaled_w = 1;
    }
    if (scaled_h < 1) {
        scaled_h = 1;
    }
    p->scaled_w = scaled_w;
    p->scaled_h = scaled_h;
    p->offset_x = (P3A_DISPLAY_WIDTH - scaled_w) / 2;
    p->offset_y = (P3A_DISPLAY_HEIGHT - scaled_h) / 2;
    const bool borders = scaled_w < P3A_DISPLAY_WIDTH || scaled_h < P3A_DISPLAY_HEIGHT;

    if (p->use_ppa) {
        p->last_err = display_ppa_upscale_rgb(p->pixels, p->w, p->h, buffer, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT,
                                              (display_rotation_t)rotation);
    } else {
        // Nearest neighbour: destination index i takes source index i * src / dst.
        // For a quarter turn the upscaler reads lookup_x by row and lookup_y by column.
        const int nx = quarter ? scaled_h : scaled_w;
        const int ny = quarter ? scaled_w : scaled_h;
        for (int i = 0; i < nx; i++) {
            s_lookup_x[i] = (uint16_t)((int64_t)i * p->w / nx);
        }
        for (int i = 0; i < ny; i++) {
            s_lookup_y[i] = (uint16_t)((int64_t)i * p->h / ny);
        }
        display_renderer_parallel_upscale_rgb(p->pixels, p->w, p->h, buffer, s_lookup_x, s_lookup_y, p->offset_x,
                                              p->offset_y, scaled_w, scaled_h, borders, (display_rotation_t)rotation);
        p->last_err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    s_last_rotation = rotation;
    s_last_background = background;
    atomic_store(&s_drawn_into, buffer);
    atomic_fetch_add(&s_frames_drawn, 1);
    return 100;
}

// Waits until a frame drawn after `since` is the one on the panel.
static bool wait_displayed(unsigned since)
{
    for (int waited_ms = 0; waited_ms < 3000; waited_ms += 5) {
        uint8_t *drawn = atomic_load(&s_drawn_into);
        if (atomic_load(&s_frames_drawn) > since && drawn != NULL && display_renderer_displayed_buffer() == drawn) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static void reply_placement(p3a_http_request_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    picture_t p = s_picture;
    xSemaphoreGive(s_lock);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "scaled_w", p.scaled_w);
    cJSON_AddNumberToObject(data, "scaled_h", p.scaled_h);
    cJSON_AddNumberToObject(data, "offset_x", p.offset_x);
    cJSON_AddNumberToObject(data, "offset_y", p.offset_y);
    cJSON_AddNumberToObject(data, "rotation", display_renderer_get_rotation());
    cJSON_AddNumberToObject(data, "frames_drawn", atomic_load(&s_frames_drawn));
    cJSON_AddStringToObject(data, "upscale", p.last_err == ESP_OK ? "ok" : p3a_err_to_name(p.last_err));
    p3a_http_ok(req, data);
}

static void post_render(p3a_http_request_t *req)
{
    httpd_req_t *raw = p3a_http_raw(req);
    char text[12];
    int w = p3a_http_query(req, "w", text, sizeof(text)) ? atoi(text) : 0;
    int h = p3a_http_query(req, "h", text, sizeof(text)) ? atoi(text) : 0;
    bool use_ppa = p3a_http_query(req, "mode", text, sizeof(text)) && strcmp(text, "ppa") == 0;
    if (w < 1 || h < 1 || w > 2048 || h > 2048 || raw->content_len != (size_t)w * (size_t)h * 3) {
        p3a_http_fail(req, 400, "INVALID", "send w, h and exactly w * h * 3 bytes of RGB");
        return;
    }
    size_t len = raw->content_len;
    uint8_t *pixels = psram_malloc(len);
    if (pixels == NULL) {
        p3a_http_fail_err(req, ESP_ERR_NO_MEM, "holding the picture");
        return;
    }
    for (size_t got = 0; got < len;) {
        int n = httpd_req_recv(raw, (char *)pixels + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(pixels);
            p3a_http_fail(req, 400, "READ_BODY", "the picture did not arrive");
            return;
        }
        got += (size_t)n;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_picture.pixels);
    s_picture = (picture_t){.pixels = pixels, .w = w, .h = h, .use_ppa = use_ppa};
    xSemaphoreGive(s_lock);

    unsigned before = atomic_load(&s_frames_drawn);
    atomic_store(&s_fresh, true);
    display_renderer_set_source(DISPLAY_SOURCE_ARTWORK, picture_source, NULL);
    display_renderer_note_content_discontinuity();
    esp_err_t err = display_renderer_select_source(DISPLAY_SOURCE_ARTWORK);
    if (err != ESP_OK || !wait_displayed(before)) {
        p3a_http_fail(req, 504, "TIMEOUT", "the picture did not reach the panel");
        return;
    }
    reply_placement(req);
}

static void post_blank(p3a_http_request_t *req)
{
    char text[4];
    bool on = p3a_http_query(req, "on", text, sizeof(text)) && text[0] == '1';
    unsigned before = atomic_load(&s_frames_drawn);
    display_renderer_set_blank(on);
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(250));  // a few frames for the black one to be on the panel
    } else if (!wait_displayed(before)) {
        p3a_http_fail(req, 504, "TIMEOUT", "the picture did not come back");
        return;
    }
    p3a_http_ok(req, NULL);
}

// The frame count, to see that a static picture is not redrawn.
static void get_render(p3a_http_request_t *req)
{
    reply_placement(req);
}

static void post_release(p3a_http_request_t *req)
{
    display_renderer_set_blank(false);
    display_renderer_set_source(DISPLAY_SOURCE_ARTWORK, NULL, NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_picture.pixels);
    s_picture = (picture_t){0};
    xSemaphoreGive(s_lock);
    p3a_http_ok(req, NULL);
}

static const p3a_http_route_t s_routes[] = {
    {P3A_HTTP_POST, P3A_HTTP_RAW, "/api/v2/test/render", post_render},
    {P3A_HTTP_GET, 0, "/api/v2/test/render", get_render},
    {P3A_HTTP_POST, 0, "/api/v2/test/render/blank", post_blank},
    {P3A_HTTP_POST, 0, "/api/v2/test/render/release", post_release},
};

void p3a_test_render_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_lookup_x = psram_malloc(P3A_DISPLAY_WIDTH * sizeof(uint16_t));
    s_lookup_y = psram_malloc(P3A_DISPLAY_HEIGHT * sizeof(uint16_t));
    if (s_lock == NULL || s_lookup_x == NULL || s_lookup_y == NULL) {
        ESP_LOGE("p3a_test", "the render hook did not start");
        return;
    }
    p3a_http_add_routes(s_routes, sizeof(s_routes) / sizeof(s_routes[0]));
}
