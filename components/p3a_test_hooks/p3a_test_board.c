// SPDX-License-Identifier: Apache-2.0
// Test console commands for the board layer: the panel's framebuffers, the
// backlight and the BOOT button.
//
// These write to and read from the framebuffers. They prove that the buffers
// are where the board says they are and that presenting one does not crash;
// they cannot prove what the glass shows. That is a manual check.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_lcd_panel_ops.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "p3a_test_internal.h"

static int s_shown = -1;  // the framebuffer last presented by these commands
static atomic_int s_button_presses;
static bool s_button_watched;

static void on_button(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_button_presses, 1);
}

static void put_pixel(uint8_t *row, int x, uint8_t r, uint8_t g, uint8_t b)
{
    row[x * 3 + 0] = b;  // framebuffer byte order is blue, green, red
    row[x * 3 + 1] = g;
    row[x * 3 + 2] = r;
}

// Picks the next framebuffer, lets `paint` fill it, presents it.
static esp_err_t paint_and_present(void (*paint)(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b))
{
    const uint8_t count = p3a_board_get_buffer_count();
    if (count == 0 || !p3a_board_get_panel()) {
        return ESP_ERR_INVALID_STATE;
    }
    const int index = (s_shown + 1) % count;
    uint8_t *buffer = p3a_board_get_buffer(index);
    const size_t stride = p3a_board_get_row_stride();
    for (int y = 0; y < P3A_DISPLAY_HEIGHT; y++) {
        uint8_t *row = buffer + (size_t)y * stride;
        for (int x = 0; x < P3A_DISPLAY_WIDTH; x++) {
            uint8_t r, g, b;
            paint(x, y, &r, &g, &b);
            put_pixel(row, x, r, g, b);
        }
    }
    esp_err_t err =
        esp_lcd_panel_draw_bitmap(p3a_board_get_panel(), 0, 0, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, buffer);
    if (err == ESP_OK) {
        s_shown = index;
    }
    return err;
}

static uint8_t s_fill[3];

static void paint_fill(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    (void)x;
    (void)y;
    *r = s_fill[0];
    *g = s_fill[1];
    *b = s_fill[2];
}

// Eight vertical bars, 90 pixels each: white, yellow, cyan, green, magenta,
// red, blue, black. The classic order, so a wrong channel order is obvious.
static void paint_bars(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    (void)y;
    const int bar = x / (P3A_DISPLAY_WIDTH / 8);
    *r = (bar == 0 || bar == 1 || bar == 4 || bar == 5) ? 255 : 0;
    *g = (bar == 0 || bar == 1 || bar == 2 || bar == 3) ? 255 : 0;
    *b = (bar == 0 || bar == 2 || bar == 4 || bar == 6) ? 255 : 0;
}

// A gradient that is different at every pixel in both directions, plus a
// white one-pixel border and a red block in the top-left corner: shows
// orientation, edges and addressing mistakes.
static void paint_gradient(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (x == 0 || y == 0 || x == P3A_DISPLAY_WIDTH - 1 || y == P3A_DISPLAY_HEIGHT - 1) {
        *r = *g = *b = 255;
    } else if (x < 60 && y < 60) {
        *r = 255;
        *g = *b = 0;
    } else {
        *r = (uint8_t)(x * 255 / (P3A_DISPLAY_WIDTH - 1));
        *g = (uint8_t)(y * 255 / (P3A_DISPLAY_HEIGHT - 1));
        *b = (uint8_t)((x + y) & 0xFF);
    }
}

static const uint8_t *shown_buffer(void)
{
    return s_shown < 0 ? NULL : p3a_board_get_buffer(s_shown);
}

static void reply_shown(esp_err_t err)
{
    if (err != ESP_OK) {
        p3a_test_reply_err(p3a_err_to_name(err));
        return;
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"buffer\":%d}", s_shown);
    p3a_test_reply_ok(json);
}

// !lcd info | fill <r> <g> <b> | bars | gradient | pixel <x> <y> | crc |
//      shot <divisor> | brightness <percent>
static void cmd_lcd(const char *args)
{
    char verb[12] = {0};
    int a = 0, b = 0, c = 0;
    const int n = sscanf(args, "%11s %d %d %d", verb, &a, &b, &c);
    if (n < 1) {
        p3a_test_reply_err("usage: !lcd <info|fill|bars|gradient|pixel|crc|shot|brightness> ...");
        return;
    }

    if (strcmp(verb, "info") == 0) {
        char json[200];
        snprintf(json, sizeof(json),
                 "{\"width\":%d,\"height\":%d,\"buffers\":%u,\"stride\":%u,\"buffer_bytes\":%u,\"brightness\":%d,"
                 "\"panel\":%s,\"shown\":%d}",
                 P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, (unsigned)p3a_board_get_buffer_count(),
                 (unsigned)p3a_board_get_row_stride(), (unsigned)p3a_board_get_buffer_bytes(),
                 p3a_board_get_brightness(), p3a_board_get_panel() ? "true" : "false", s_shown);
        p3a_test_reply_ok(json);
    } else if (strcmp(verb, "fill") == 0 && n == 4) {
        s_fill[0] = (uint8_t)a;
        s_fill[1] = (uint8_t)b;
        s_fill[2] = (uint8_t)c;
        reply_shown(paint_and_present(paint_fill));
    } else if (strcmp(verb, "bars") == 0) {
        reply_shown(paint_and_present(paint_bars));
    } else if (strcmp(verb, "gradient") == 0) {
        reply_shown(paint_and_present(paint_gradient));
    } else if (strcmp(verb, "pixel") == 0 && n == 3) {
        const uint8_t *buffer = shown_buffer();
        if (!buffer || a < 0 || a >= P3A_DISPLAY_WIDTH || b < 0 || b >= P3A_DISPLAY_HEIGHT) {
            p3a_test_reply_err("nothing shown yet, or coordinates out of range");
            return;
        }
        const uint8_t *px = buffer + (size_t)b * p3a_board_get_row_stride() + (size_t)a * 3;
        char json[64];
        snprintf(json, sizeof(json), "{\"r\":%u,\"g\":%u,\"b\":%u}", px[2], px[1], px[0]);
        p3a_test_reply_ok(json);
    } else if (strcmp(verb, "crc") == 0) {
        const uint8_t *buffer = shown_buffer();
        if (!buffer) {
            p3a_test_reply_err("nothing shown yet");
            return;
        }
        // Over the visible bytes of each row, so that stride padding does not matter.
        uint32_t crc = 0;
        for (int y = 0; y < P3A_DISPLAY_HEIGHT; y++) {
            crc = esp_rom_crc32_le(crc, buffer + (size_t)y * p3a_board_get_row_stride(), P3A_DISPLAY_WIDTH * 3);
        }
        char json[48];
        snprintf(json, sizeof(json), "{\"crc\":%u}", (unsigned)crc);
        p3a_test_reply_ok(json);
    } else if (strcmp(verb, "shot") == 0 && n == 2) {
        // A downscaled copy of what is shown: one TEST|data| line of hex
        // (red, green, blue per pixel) per row, then the reply.
        const uint8_t *buffer = shown_buffer();
        if (!buffer || a < 1 || a > 90 || P3A_DISPLAY_WIDTH % a != 0) {
            p3a_test_reply_err("nothing shown yet, or the divisor does not divide 720 (1..90)");
            return;
        }
        static char line[P3A_DISPLAY_WIDTH * 6 + 1];
        static const char hex[] = "0123456789abcdef";
        const int out_w = P3A_DISPLAY_WIDTH / a, out_h = P3A_DISPLAY_HEIGHT / a;
        for (int oy = 0; oy < out_h; oy++) {
            const uint8_t *row = buffer + (size_t)(oy * a + a / 2) * p3a_board_get_row_stride();
            char *out = line;
            for (int ox = 0; ox < out_w; ox++) {
                const uint8_t *px = row + (size_t)(ox * a + a / 2) * 3;
                const uint8_t rgb[3] = {px[2], px[1], px[0]};
                for (int k = 0; k < 3; k++) {
                    *out++ = hex[rgb[k] >> 4];
                    *out++ = hex[rgb[k] & 0xF];
                }
            }
            *out = '\0';
            printf("TEST|data|%s\n", line);
            // Console output is a busy wait. A large shot takes longer than the
            // task watchdog lets the idle task starve, so let it run.
            vTaskDelay(1);
        }
        char json[64];
        snprintf(json, sizeof(json), "{\"width\":%d,\"height\":%d}", out_w, out_h);
        p3a_test_reply_ok(json);
    } else if (strcmp(verb, "brightness") == 0 && n == 2) {
        esp_err_t err = p3a_board_set_brightness(a);
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
            return;
        }
        char json[48];
        snprintf(json, sizeof(json), "{\"brightness\":%d}", p3a_board_get_brightness());
        p3a_test_reply_ok(json);
    } else {
        p3a_test_reply_err("usage: !lcd <info|fill r g b|bars|gradient|pixel x y|crc|shot div|brightness pct>");
    }
}

// !button   starts counting BOOT presses on first use and reports the count.
static void cmd_button(const char *args)
{
    (void)args;
    if (!s_button_watched) {
        esp_err_t err = p3a_board_button_init(on_button, NULL);
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
            return;
        }
        s_button_watched = true;
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"presses\":%d}", atomic_load(&s_button_presses));
    p3a_test_reply_ok(json);
}

const p3a_test_command_t p3a_test_board_commands[] = {
    {"lcd", cmd_lcd},
    {"button", cmd_button},
};
const size_t p3a_test_board_command_count = sizeof(p3a_test_board_commands) / sizeof(p3a_test_board_commands[0]);
