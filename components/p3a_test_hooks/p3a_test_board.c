// SPDX-License-Identifier: Apache-2.0
// Test console commands for the board layer: the panel's framebuffers, the
// backlight and the BOOT button.
//
// The pattern commands draw through the display pipeline (as its test
// source) and read the framebuffer it reports as displayed. They prove that
// what was drawn is what the panel is given; they cannot prove what the
// glass shows. That is a manual check.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "display_renderer.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_board.h"
#include "p3a_err.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"
#include "sd_idle_wait.h"
#include "sdio_bus.h"
#include "sdmmc_cmd.h"
#include "slave_ota.h"

static int s_shown = -1;  // the framebuffer last presented by these commands

static void put_pixel(uint8_t *row, int x, uint8_t r, uint8_t g, uint8_t b)
{
    row[x * 3 + 0] = b;  // framebuffer byte order is blue, green, red
    row[x * 3 + 1] = g;
    row[x * 3 + 2] = r;
}

// The patterns reach the panel the way everything does: through the display
// pipeline. These commands are a frame source in the pipeline's test slot.
typedef void (*painter_t)(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b);

static painter_t s_painter;
static atomic_bool s_paint_wanted;
static _Atomic(uint8_t *) s_painted_into;  // the framebuffer the pattern was last drawn into

static int test_pattern_source(uint8_t *buffer, bool must_redraw, void *ctx)
{
    (void)ctx;
    painter_t paint = s_painter;
    if (paint == NULL) {
        memset(buffer, 0, p3a_board_get_buffer_bytes());
        return 100;
    }
    if (!atomic_exchange(&s_paint_wanted, false) && !must_redraw) {
        return DISPLAY_FRAME_UNCHANGED;
    }
    const size_t stride = p3a_board_get_row_stride();
    for (int y = 0; y < P3A_DISPLAY_HEIGHT; y++) {
        uint8_t *row = buffer + (size_t)y * stride;
        for (int x = 0; x < P3A_DISPLAY_WIDTH; x++) {
            uint8_t r, g, b;
            paint(x, y, &r, &g, &b);
            put_pixel(row, x, r, g, b);
        }
    }
    atomic_store(&s_painted_into, buffer);
    return 100;
}

// Has the pipeline draw `paint` and waits until that frame is the one the
// panel is scanning out.
static esp_err_t paint_and_present(painter_t paint)
{
    if (!p3a_board_get_panel()) {
        return ESP_ERR_INVALID_STATE;
    }
    static bool registered;
    if (!registered) {
        display_renderer_set_source(DISPLAY_SOURCE_TEST, test_pattern_source, NULL);
        registered = true;
    }
    s_painter = paint;
    atomic_store(&s_painted_into, NULL);
    atomic_store(&s_paint_wanted, true);
    esp_err_t err = display_renderer_select_source(DISPLAY_SOURCE_TEST);
    if (err != ESP_OK) {
        return err;
    }
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 5) {
        uint8_t *drawn = atomic_load(&s_painted_into);
        if (drawn != NULL && display_renderer_displayed_buffer() == drawn) {
            for (int i = 0; i < p3a_board_get_buffer_count(); i++) {
                if (p3a_board_get_buffer(i) == drawn) {
                    s_shown = i;
                }
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
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

// What the pattern commands last put on the panel.
static const uint8_t *shown_buffer(void)
{
    return s_shown < 0 ? NULL : p3a_board_get_buffer(s_shown);
}

// What the panel is showing now, whoever drew it.
const uint8_t *p3a_test_shown_buffer(void)
{
    return display_renderer_displayed_buffer();
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

// !button   reports how many BOOT presses the board has seen (app_main watches the button).
static void cmd_button(const char *args)
{
    (void)args;
    char json[48];
    snprintf(json, sizeof(json), "{\"presses\":%u}", (unsigned)p3a_board_button_presses());
    p3a_test_reply_ok(json);
}

// !touch   reads the touch controller once and reports the points it sees.
static void cmd_touch(const char *args)
{
    (void)args;
    esp_lcd_touch_handle_t touch = p3a_board_get_touch();
    if (!touch) {
        p3a_test_reply_ok("{\"ready\":false,\"points\":0}");
        return;
    }
    esp_lcd_touch_point_data_t points[5] = {0};
    uint8_t count = 0;
    esp_err_t err = esp_lcd_touch_read_data(touch);
    if (err == ESP_OK) {
        err = esp_lcd_touch_get_data(touch, points, &count, 5);
    }
    if (err != ESP_OK) {
        p3a_test_reply_err(p3a_err_to_name(err));
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"ready\":true,\"points\":%u,\"x\":%u,\"y\":%u}", (unsigned)count,
             count ? (unsigned)points[0].x : 0u, count ? (unsigned)points[0].y : 0u);
    p3a_test_reply_ok(json);
}

// !sd info          the card, and which SD fixes this build carries
// !sd rw <bytes>    writes a pattern to a scratch file, reads it back, deletes it
static void cmd_sd(const char *args)
{
    char verb[8] = {0};
    int bytes = 0;
    const int n = sscanf(args, "%7s %d", verb, &bytes);

    if (n >= 1 && strcmp(verb, "info") == 0) {
        sdmmc_card_t *card = p3a_board_sd_card();
        // FatFS must get its buffers from the aligned allocator, or every SD
        // transfer bounces through a 4 KB internal-RAM buffer.
        void *probe = ff_memalloc(4096);
        const bool aligned = probe && ((uintptr_t)probe % 64) == 0;
        const bool in_psram = probe && esp_ptr_external_ram(probe);
        ff_memfree(probe);
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"mounted\":%s,\"capacity_mb\":%u,\"name\":\"%.5s\",\"ff_aligned\":%s,\"ff_psram\":%s,"
                 "\"idle_wait_wrap\":%s,\"idf_patched\":%s,\"bus_locked\":%s}",
                 card ? "true" : "false",
                 card ? (unsigned)(((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024)) : 0u,
                 card ? card->cid.name : "", aligned ? "true" : "false", in_psram ? "true" : "false",
                 sd_idle_wait_wrap_enabled() ? "true" : "false", sd_idle_wait_idf_patched() ? "true" : "false",
                 sdio_bus_is_locked() ? "true" : "false");
        p3a_test_reply_ok(json);
        return;
    }
    if (n == 2 && strcmp(verb, "rw") == 0 && bytes > 0 && bytes <= 4 * 1024 * 1024) {
        if (!p3a_board_sd_is_mounted()) {
            p3a_test_reply_err("no card mounted");
            return;
        }
        static const char *path = P3A_SD_MOUNT_POINT "/p3a_test.bin";
        uint8_t *out = psram_malloc((size_t)bytes);
        uint8_t *in = psram_malloc((size_t)bytes);
        const char *failure = NULL;
        int64_t t0 = 0, t1 = 0, t2 = 0;
        if (!out || !in) {
            failure = "no memory";
        } else {
            for (int i = 0; i < bytes; i++) {
                out[i] = (uint8_t)((i * 31) ^ (i >> 8));
            }
            t0 = esp_timer_get_time();
            FILE *f = fopen(path, "wb");
            if (!f || fwrite(out, 1, (size_t)bytes, f) != (size_t)bytes || fflush(f) != 0 || fsync(fileno(f)) != 0) {
                failure = "write failed";
            }
            if (f) {
                fclose(f);
            }
            t1 = esp_timer_get_time();
            if (!failure) {
                f = fopen(path, "rb");
                if (!f || fread(in, 1, (size_t)bytes, f) != (size_t)bytes) {
                    failure = "read failed";
                } else if (memcmp(in, out, (size_t)bytes) != 0) {
                    failure = "read back different bytes";
                }
                if (f) {
                    fclose(f);
                }
            }
            t2 = esp_timer_get_time();
            unlink(path);
        }
        free(out);
        free(in);
        if (failure) {
            p3a_test_reply_err(failure);
            return;
        }
        char json[96];
        snprintf(json, sizeof(json), "{\"bytes\":%d,\"write_ms\":%d,\"read_ms\":%d}", bytes, (int)((t1 - t0) / 1000),
                 (int)((t2 - t1) / 1000));
        p3a_test_reply_ok(json);
        return;
    }
    p3a_test_reply_err("usage: !sd <info|rw bytes>");
}

// !c6   the Wi-Fi co-processor: what it runs, what this firmware carries for it
static void cmd_c6(const char *args)
{
    (void)args;
    uint32_t run[3] = {0}, emb[3] = {0};
    const bool running_known = slave_ota_get_running_version(&run[0], &run[1], &run[2]) == ESP_OK;
    slave_ota_get_embedded_version(&emb[0], &emb[1], &emb[2]);

    // The image that an update would send: is it there, and is it an app image?
    bool image_ok = false;
    char image_project[32] = "";
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "slave_fw");
    if (part) {
        esp_app_desc_t desc;
        if (esp_partition_read(part, sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), &desc,
                               sizeof(desc)) == ESP_OK &&
            desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
            image_ok = true;
            snprintf(image_project, sizeof(image_project), "%.31s", desc.project_name);
        }
    }
    char json[256];
    snprintf(json, sizeof(json),
             "{\"running_known\":%s,\"running\":\"%u.%u.%u\",\"embedded\":\"%u.%u.%u\",\"locked\":%s,"
             "\"updating\":%s,\"partition\":%s,\"image_ok\":%s,\"image_project\":\"%s\"}",
             running_known ? "true" : "false", (unsigned)run[0], (unsigned)run[1], (unsigned)run[2], (unsigned)emb[0],
             (unsigned)emb[1], (unsigned)emb[2], slave_ota_is_version_locked() ? "true" : "false",
             slave_ota_is_in_progress() ? "true" : "false", part ? "true" : "false", image_ok ? "true" : "false",
             image_project);
    p3a_test_reply_ok(json);
}

const p3a_test_command_t p3a_test_board_commands[] = {
    {"c6", cmd_c6},
    {"lcd", cmd_lcd},
    {"button", cmd_button},
    {"touch", cmd_touch},
    {"sd", cmd_sd},
};
const size_t p3a_test_board_command_count = sizeof(p3a_test_board_commands) / sizeof(p3a_test_board_commands[0]);
