// SPDX-License-Identifier: Apache-2.0
// The BMP decoder on the workstation, under the sanitizers.
//
// Every other format is decoded by a library; BMP is p3a's own parser, and it
// reads bytes that come from anywhere. So it is run here on the corpus, on
// every truncation of every good file, and on thousands of mutated copies.
// The assertions are few: the point is that the sanitizers stay silent and
// that nothing announced is larger than the decoder's own limit.
#include "animation_decoder.h"
#include "animation_decoder_formats.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What the decoder asks its component for.
static uint8_t s_bg[3] = {0x20, 0x40, 0x60};

void animation_decoder_get_background(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_bg[0];
    *g = s_bg[1];
    *b = s_bg[2];
}

void setUp(void) {}
void tearDown(void) {}

static uint8_t *read_file(const char *folder, const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/bmp/%s", P3A_DECODER_CORPUS, folder, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    TEST_ASSERT_EQUAL((size_t)size, fread(data, 1, (size_t)size, f));
    fclose(f);
    *len = (size_t)size;
    return data;
}

// Opens, describes, decodes and closes. Returns the init result. The bytes
// are copied into a block of exactly their size first, so that a read past
// the end is a read past an allocation, which the sanitizer reports.
static esp_err_t exercise(const uint8_t *bytes, size_t len, animation_decoder_info_t *info_out, uint8_t **pixels_out)
{
    uint8_t *exact = malloc(len > 0 ? len : 1);
    memcpy(exact, bytes, len);
    animation_decoder_t *decoder = NULL;
    esp_err_t err = len > 0 ? bmp_decoder_init(&decoder, exact, len) : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        TEST_ASSERT_NOT_NULL(decoder);
        animation_decoder_info_t info;
        memset(&info, 0, sizeof(info));
        TEST_ASSERT_EQUAL(ESP_OK, bmp_decoder_get_info(decoder, &info));
        TEST_ASSERT_TRUE(info.canvas_width >= 1 && info.canvas_width <= ANIMATION_DECODER_MAX_DIMENSION);
        TEST_ASSERT_TRUE(info.canvas_height >= 1 && info.canvas_height <= ANIMATION_DECODER_MAX_DIMENSION);
        TEST_ASSERT_EQUAL(1, info.frame_count);
        size_t bytes_out = (size_t)info.canvas_width * info.canvas_height * 3;
        uint8_t *pixels = malloc(bytes_out);  // exactly the promised size
        TEST_ASSERT_NOT_NULL(pixels);
        esp_err_t decoded = bmp_decoder_decode_next_rgb(decoder, pixels);
        uint32_t delay = 0;
        TEST_ASSERT_EQUAL(ESP_OK, bmp_decoder_get_frame_delay(decoder, &delay));
        TEST_ASSERT_EQUAL(ANIMATION_DECODER_STILL_FRAME_DELAY_MS, delay);
        bmp_decoder_reset(decoder);
        if (info_out != NULL) {
            *info_out = info;
        }
        if (pixels_out != NULL && decoded == ESP_OK) {
            *pixels_out = pixels;
        } else {
            free(pixels);
        }
        bmp_decoder_unload(&decoder);
    } else {
        TEST_ASSERT_NULL(decoder);
    }
    free(exact);
    return err;
}

static const char *const GOOD[] = {"rgb24.bmp", "rgba32.bmp", "palette8.bmp", "mono1.bmp", "odd_width.bmp"};
#define GOOD_COUNT (sizeof(GOOD) / sizeof(GOOD[0]))

static void test_good_files_decode(void)
{
    static const struct {
        const char *name;
        uint32_t w, h;
    } expect[] = {{"rgb24.bmp", 48, 32}, {"rgba32.bmp", 40, 40}, {"palette8.bmp", 32, 32}, {"mono1.bmp", 33, 9},
                  {"odd_width.bmp", 31, 5}};
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        size_t len;
        uint8_t *file = read_file("good", expect[i].name, &len);
        animation_decoder_info_t info;
        uint8_t *pixels = NULL;
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, exercise(file, len, &info, &pixels), expect[i].name);
        TEST_ASSERT_EQUAL(expect[i].w, info.canvas_width);
        TEST_ASSERT_EQUAL(expect[i].h, info.canvas_height);
        TEST_ASSERT_TRUE(info.source_consumed);
        TEST_ASSERT_NOT_NULL_MESSAGE(pixels, expect[i].name);
        free(pixels);
        free(file);
    }
}

// The scene every corpus picture is drawn from puts pure red at this spot
// (tests/assets/make_corpus.py): a check that channels and row order are right.
static void test_a_known_pixel(void)
{
    size_t len;
    uint8_t *file = read_file("good", "rgb24.bmp", &len);
    animation_decoder_info_t info;
    uint8_t *pixels = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, exercise(file, len, &info, &pixels));
    const uint8_t *in_the_red_box = pixels + ((size_t)6 * 48 + 8) * 3;  // x 8, y 6
    TEST_ASSERT_EQUAL_UINT8(255, in_the_red_box[0]);
    TEST_ASSERT_EQUAL_UINT8(40, in_the_red_box[1]);
    TEST_ASSERT_EQUAL_UINT8(40, in_the_red_box[2]);
    const uint8_t *top_left = pixels;  // the gradient starts dark at the top left
    TEST_ASSERT_EQUAL_UINT8(0, top_left[0]);
    TEST_ASSERT_EQUAL_UINT8(0, top_left[1]);
    free(pixels);
    free(file);
}

static void test_bad_files_are_refused(void)
{
    static const char *const refused[] = {"empty_ish.bmp", "not_an_image.bmp", "bmp_truncated_header.bmp",
                                          "claims_100000x100000.bmp"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        size_t len;
        uint8_t *file = read_file("bad", refused[i], &len);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(ESP_OK, exercise(file, len, NULL, NULL), refused[i]);
        free(file);
    }
    // These may open or not; either way without harm.
    static const char *const either[] = {"bmp_truncated_half.bmp", "bmp_damaged_middle.bmp",
                                         "claims_more_pixels_than_it_has.bmp"};
    for (size_t i = 0; i < sizeof(either) / sizeof(either[0]); i++) {
        size_t len;
        uint8_t *file = read_file("bad", either[i], &len);
        exercise(file, len, NULL, NULL);
        free(file);
    }
}

static void test_every_truncation_of_every_good_file(void)
{
    for (size_t g = 0; g < GOOD_COUNT; g++) {
        size_t len;
        uint8_t *file = read_file("good", GOOD[g], &len);
        for (size_t cut = 0; cut < len; cut++) {
            exercise(file, cut, NULL, NULL);
        }
        free(file);
    }
}

static uint32_t s_rng = 0x9E3779B9u;

static uint32_t next_random(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

// A header field set to an extreme is the classic way to break a parser.
static void test_every_header_byte_at_its_extremes(void)
{
    static const uint8_t extremes[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
    for (size_t g = 0; g < GOOD_COUNT; g++) {
        size_t len;
        uint8_t *file = read_file("good", GOOD[g], &len);
        size_t header = len < 140 ? len : 140;
        for (size_t at = 0; at < header; at++) {
            uint8_t kept = file[at];
            for (size_t e = 0; e < sizeof(extremes); e++) {
                file[at] = extremes[e];
                exercise(file, len, NULL, NULL);
            }
            file[at] = kept;
        }
        free(file);
    }
}

static void test_random_damage(void)
{
    for (size_t g = 0; g < GOOD_COUNT; g++) {
        size_t len;
        uint8_t *file = read_file("good", GOOD[g], &len);
        uint8_t *copy = malloc(len);
        for (int round = 0; round < 3000; round++) {
            memcpy(copy, file, len);
            int hits = 1 + (int)(next_random() % 4);
            for (int h = 0; h < hits; h++) {
                // Mostly in the headers, where the decisions are made.
                size_t span = (next_random() % 4 != 0 && len > 64) ? 64 : len;
                copy[next_random() % span] = (uint8_t)next_random();
            }
            exercise(copy, len, NULL, NULL);
        }
        free(copy);
        free(file);
    }
}

// --- the frame delay rule (decision D37) ------------------------------------------

static void test_short_delays_are_shown_as_browsers_show_them(void)
{
    TEST_ASSERT_EQUAL_UINT32(100, animation_decoder_effective_delay_ms(0));
    TEST_ASSERT_EQUAL_UINT32(100, animation_decoder_effective_delay_ms(1));
    TEST_ASSERT_EQUAL_UINT32(100, animation_decoder_effective_delay_ms(10));
    TEST_ASSERT_EQUAL_UINT32(11, animation_decoder_effective_delay_ms(11));
    TEST_ASSERT_EQUAL_UINT32(20, animation_decoder_effective_delay_ms(20));
    TEST_ASSERT_EQUAL_UINT32(100, animation_decoder_effective_delay_ms(100));
    TEST_ASSERT_EQUAL_UINT32(65535, animation_decoder_effective_delay_ms(65535));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, animation_decoder_effective_delay_ms(0xFFFFFFFFu));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_good_files_decode);
    RUN_TEST(test_a_known_pixel);
    RUN_TEST(test_bad_files_are_refused);
    RUN_TEST(test_every_truncation_of_every_good_file);
    RUN_TEST(test_every_header_byte_at_its_extremes);
    RUN_TEST(test_random_damage);
    RUN_TEST(test_short_delays_are_shown_as_browsers_show_them);
    return UNITY_END();
}
