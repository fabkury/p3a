// SPDX-License-Identifier: Apache-2.0
// Klipy's listings read into records, on responses recorded from the
// service and against what a Python reading of the same files expects
// (tests/assets/sources/make_expected.py); the addresses p3a asks for; and
// the parser on cut and mutated input, for the sanitizers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klipy_parse.h"
#include "p3a_err.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define NOW 1700000000u

static char *read_file(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", P3A_SOURCE_ASSETS, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    TEST_ASSERT_EQUAL((size_t)size, fread(text, 1, (size_t)size, f));
    text[size] = '\0';
    fclose(f);
    if (len != NULL) {
        *len = (size_t)size;
    }
    return text;
}

static klipy_parse_opts_t opts_for(uint8_t product, bool webp)
{
    return (klipy_parse_opts_t){.product = product, .webp = webp, .screen_w = 720, .screen_h = 720, .now = NOW};
}

static void check_page(const char *listing, const char *expected_name, uint8_t product, bool webp)
{
    char *json = read_file(listing, NULL), *expected = read_file(expected_name, NULL);
    p3a_record_t *records = calloc(64, sizeof(p3a_record_t));
    const klipy_parse_opts_t opts = opts_for(product, webp);
    uint32_t count = 0, items = 0;
    bool has_next = false;
    TEST_ASSERT_EQUAL(ESP_OK, klipy_parse_page(json, &opts, records, 64, &count, &items, &has_next));
    char *line = strtok(expected, "\n");
    unsigned want_items, want_next;
    TEST_ASSERT_EQUAL(2, sscanf(line, "%u %u", &want_items, &want_next));
    TEST_ASSERT_EQUAL_UINT32(want_items, items);
    TEST_ASSERT_EQUAL(want_next != 0, has_next);
    uint32_t at = 0;
    while ((line = strtok(NULL, "\n")) != NULL) {
        unsigned long long id;
        unsigned w, h;
        TEST_ASSERT_EQUAL(3, sscanf(line, "%llu %u %u", &id, &w, &h));
        TEST_ASSERT_TRUE_MESSAGE(at < count, "fewer records than expected");
        const p3a_record_t *r = &records[at++];
        TEST_ASSERT_EQUAL_UINT64(id, r->identity.klipy.id);
        TEST_ASSERT_EQUAL(product, r->identity.klipy.product);
        TEST_ASSERT_EQUAL_MESSAGE(w, r->width, line);
        TEST_ASSERT_EQUAL_MESSAGE(h, r->height, line);
        TEST_ASSERT_EQUAL_UINT32(NOW, r->sort_at);
        TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(r));
    }
    TEST_ASSERT_EQUAL_UINT32(at, count);
    TEST_ASSERT_TRUE_MESSAGE(count >= 20, "the recorded page holds a real listing");
    free(records);
    free(json);
    free(expected);
}

static void test_recorded_listings_are_read_as_an_independent_reading_expects(void)
{
    check_page("klipy_gifs_trending.json", "klipy_gifs_trending.gif.expected", KLIPY_PRODUCT_GIF, false);
    check_page("klipy_gifs_trending.json", "klipy_gifs_trending.webp.expected", KLIPY_PRODUCT_GIF, true);
    check_page("klipy_stickers_search.json", "klipy_stickers_search.gif.expected", KLIPY_PRODUCT_STICKER, false);
    check_page("klipy_stickers_search.json", "klipy_stickers_search.webp.expected", KLIPY_PRODUCT_STICKER, true);
}

static void check_item(const char *expected_name, bool webp)
{
    char *json = read_file("klipy_gifs_item.json", NULL), *expected = read_file(expected_name, NULL);
    const klipy_parse_opts_t opts = opts_for(KLIPY_PRODUCT_GIF, webp);
    char url[320];
    TEST_ASSERT_EQUAL(ESP_OK, klipy_parse_item_url(json, &opts, url, sizeof(url)));
    strtok(expected, "\n");
    const char *line = strtok(NULL, "\n");
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQUAL_STRING(strrchr(line, ' ') + 1, url);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, klipy_parse_item_url(json, &opts, url, 24));
    free(json);
    free(expected);
}

static void test_the_address_of_an_items_file_is_read_from_the_recorded_answer(void)
{
    check_item("klipy_gifs_item.gif.expected", false);
    check_item("klipy_gifs_item.webp.expected", true);
    // Klipy no longer has it: an empty list, which is not damage.
    const klipy_parse_opts_t opts = opts_for(KLIPY_PRODUCT_GIF, false);
    char url[320];
    char *gone = read_file("klipy_gifs_item_missing.json", NULL);
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, klipy_parse_item_url(gone, &opts, url, sizeof(url)));
    free(gone);
    char *refused = read_file("klipy_bad_key.json", NULL);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, klipy_parse_item_url(refused, &opts, url, sizeof(url)));
    free(refused);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, klipy_parse_item_url("{\"data\":{\"data\":[{\"id\":5}]}}", &opts, url, sizeof(url)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, klipy_parse_item_url("", &opts, url, sizeof(url)));
}

static void test_an_empty_listing_and_a_refusal_are_told_apart_from_damage(void)
{
    const klipy_parse_opts_t opts = opts_for(KLIPY_PRODUCT_GIF, false);
    p3a_record_t records[4];
    uint32_t count = 9, items = 9;
    bool has_next = true;
    char *empty = read_file("klipy_gifs_search_empty.json", NULL);
    TEST_ASSERT_EQUAL(ESP_OK, klipy_parse_page(empty, &opts, records, 4, &count, &items, &has_next));
    TEST_ASSERT_EQUAL(0, items);
    TEST_ASSERT_FALSE(has_next);
    free(empty);
    char *refused = read_file("klipy_bad_key.json", NULL);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, klipy_parse_page(refused, &opts, records, 4, &count, &items, &has_next));
    free(refused);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, klipy_parse_page("", &opts, records, 4, &count, &items, &has_next));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, klipy_parse_page("{\"data\":[]}", &opts, records, 4, &count, &items, &has_next));
}

#define FILE_OF(url, w, h) "{\"url\":\"" url "\",\"width\":" #w ",\"height\":" #h "}"

static void test_which_file_is_taken_and_which_items_are_skipped(void)
{
    static const char *json =
        "{\"result\":true,\"data\":{\"data\":["
        // md is larger than hd, and both fit: md. The format asked for is GIF.
        "{\"id\":11,\"type\":\"gif\",\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com/a/hd.gif", 300, 300)
        ",\"webp\":" FILE_OF("https://static.klipy.com/a/hd.webp", 900, 900) "},"
        "\"md\":{\"gif\":" FILE_OF("https://static.klipy.com/a/md.gif", 500, 400) "}}},"
        // hd is more than a screen and a half: the largest that fits.
        "{\"id\":\"12\",\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com/b/hd.gif", 1920, 1080) "},"
        "\"sm\":{\"gif\":" FILE_OF("https://static.klipy.com/b/sm.gif", 1080, 608) "},"
        "\"xs\":{\"gif\":" FILE_OF("https://static.klipy.com/b/xs.gif", 100, 56) "}}},"
        // Nothing fits: the largest there is.
        "{\"id\":13,\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com/c/hd.gif", 4000, 3000) "},"
        "\"md\":{\"gif\":" FILE_OF("https://static.klipy.com/c/md.gif", 2000, 1500) "}}},"
        // No GIF at all: the WebP.
        "{\"id\":14,\"file\":{\"hd\":{\"webp\":" FILE_OF("https://static.klipy.com/d/hd.webp", 320, 240) "}}},"
        // Skipped: an advertisement, no id, an id of zero, no file, a file elsewhere, not over https.
        "{\"id\":15,\"type\":\"ad\",\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com/e.gif", 10, 10) "}}},"
        "{\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com/f.gif", 10, 10) "}}},"
        "{\"id\":0,\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com/g.gif", 10, 10) "}}},"
        "{\"id\":18},"
        "{\"id\":19,\"file\":{\"hd\":{\"gif\":" FILE_OF("https://klipy.com.example.org/h.gif", 10, 10) "}}},"
        "{\"id\":20,\"file\":{\"hd\":{\"gif\":" FILE_OF("https://static.klipy.com@example.org/h.gif", 10, 10) "}}},"
        "{\"id\":21,\"file\":{\"hd\":{\"gif\":" FILE_OF("http://static.klipy.com/i.gif", 10, 10) "}}},"
        "7"
        "]}}";
    const klipy_parse_opts_t opts = opts_for(KLIPY_PRODUCT_STICKER, false);
    p3a_record_t records[8];
    uint32_t count = 0, items = 0;
    bool has_next = true;
    TEST_ASSERT_EQUAL(ESP_OK, klipy_parse_page(json, &opts, records, 8, &count, &items, &has_next));
    TEST_ASSERT_EQUAL(12, items);
    TEST_ASSERT_EQUAL(4, count);
    TEST_ASSERT_FALSE(has_next);  // the page does not say, and it is not a full one
    TEST_ASSERT_EQUAL_UINT64(11, records[0].identity.klipy.id);
    TEST_ASSERT_EQUAL(500, records[0].width);
    TEST_ASSERT_EQUAL_UINT64(12, records[1].identity.klipy.id);
    TEST_ASSERT_EQUAL(1080, records[1].width);
    TEST_ASSERT_EQUAL(4000, records[2].width);
    TEST_ASSERT_EQUAL(320, records[3].width);
    TEST_ASSERT_EQUAL(KLIPY_PRODUCT_STICKER, records[3].identity.klipy.product);
}

static void test_kinds_and_addresses(void)
{
    uint8_t product = 9;
    klipy_mode_t mode = KLIPY_MODE_SEARCH;
    TEST_ASSERT_TRUE(klipy_kind_parse("gif:trending", &product, &mode));
    TEST_ASSERT_EQUAL(KLIPY_PRODUCT_GIF, product);
    TEST_ASSERT_EQUAL(KLIPY_MODE_TRENDING, mode);
    TEST_ASSERT_TRUE(klipy_kind_parse("sticker:search", &product, &mode));
    TEST_ASSERT_EQUAL(KLIPY_PRODUCT_STICKER, product);
    TEST_ASSERT_EQUAL(KLIPY_MODE_SEARCH, mode);
    TEST_ASSERT_TRUE(klipy_kind_parse("sticker:category", &product, &mode));
    TEST_ASSERT_EQUAL(KLIPY_MODE_SEARCH, mode);
    TEST_ASSERT_TRUE(klipy_kind_parse("trending", &product, &mode));
    TEST_ASSERT_EQUAL(KLIPY_PRODUCT_GIF, product);
    TEST_ASSERT_TRUE(klipy_kind_parse("", &product, &mode));
    TEST_ASSERT_EQUAL(KLIPY_MODE_TRENDING, mode);
    TEST_ASSERT_FALSE(klipy_kind_parse(NULL, &product, &mode));
    TEST_ASSERT_FALSE(klipy_kind_parse("clip:trending", &product, &mode));
    TEST_ASSERT_FALSE(klipy_kind_parse("gif:newest", &product, &mode));
    TEST_ASSERT_FALSE(klipy_kind_parse("stickers:trending", &product, &mode));

    char url[320];
    TEST_ASSERT_EQUAL(ESP_OK, klipy_list_url("KEY9", KLIPY_PRODUCT_GIF, NULL, 1, "pg-13", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.klipy.com/api/v1/KEY9/gifs/trending?page=1&per_page=50&rating=pg-13", url);
    TEST_ASSERT_EQUAL(ESP_OK, klipy_list_url("KEY9", KLIPY_PRODUCT_STICKER, "pixel art & co", 3, "g", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.klipy.com/api/v1/KEY9/stickers/search?q=pixel%20art%20%26%20co&page=3&per_page=50&rating=g",
                             url);
    // The key is a part of the path: whatever was typed, it stays one part.
    TEST_ASSERT_EQUAL(ESP_OK, klipy_list_url("a/../b?x", KLIPY_PRODUCT_GIF, NULL, 1, "g", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.klipy.com/api/v1/a%2F..%2Fb%3Fx/gifs/trending?page=1&per_page=50&rating=g", url);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, klipy_list_url("", KLIPY_PRODUCT_GIF, NULL, 1, "g", url, sizeof(url)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, klipy_list_url("K", KLIPY_PRODUCT_GIF, NULL, 0, "g", url, sizeof(url)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, klipy_list_url("KEY9", KLIPY_PRODUCT_GIF, NULL, 1, "g", url, 40));

    p3a_record_t r;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_klipy(&r, 1209252931009909ull, KLIPY_PRODUCT_STICKER));
    TEST_ASSERT_EQUAL(ESP_OK, klipy_item_url("KEY9", &r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.klipy.com/api/v1/KEY9/stickers/items?ids=1209252931009909", url);
    p3a_record_t other;
    p3a_record_init_giphy(&other, "abc");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, klipy_item_url("KEY9", &other, url, sizeof(url)));
}

static void test_cut_and_mutated_listings_do_no_harm(void)
{
    size_t len = 0;
    char *json = read_file("klipy_gifs_trending.json", &len);
    char *copy = malloc(len + 1);
    p3a_record_t *records = calloc(64, sizeof(p3a_record_t));
    const klipy_parse_opts_t opts = opts_for(KLIPY_PRODUCT_GIF, true);
    uint32_t count, items, seed = 777;
    bool has_next;
    char url[320];
    for (size_t cut = 0; cut < len; cut += 191) {
        memcpy(copy, json, cut);
        copy[cut] = '\0';
        klipy_parse_page(copy, &opts, records, 64, &count, &items, &has_next);
        klipy_parse_item_url(copy, &opts, url, sizeof(url));
    }
    for (int round = 0; round < 1500; round++) {
        memcpy(copy, json, len + 1);
        for (int k = 0; k < 1 + round % 6; k++) {
            seed = seed * 1664525u + 1013904223u;
            const size_t at = (seed >> 8) % len;
            seed = seed * 1664525u + 1013904223u;
            copy[at] = (char)(1 + (seed >> 16) % 255);
        }
        if (klipy_parse_page(copy, &opts, records, 64, &count, &items, &has_next) == ESP_OK) {
            TEST_ASSERT_TRUE(count <= items && count <= 64);
            for (uint32_t i = 0; i < count; i++) {
                TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&records[i]));
            }
        }
        if (klipy_parse_item_url(copy, &opts, url, sizeof(url)) == ESP_OK) {
            TEST_ASSERT_EQUAL(0, strncmp(url, "https://", 8));  // whatever was changed, never another scheme
        }
    }
    free(records);
    free(copy);
    free(json);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_recorded_listings_are_read_as_an_independent_reading_expects);
    RUN_TEST(test_the_address_of_an_items_file_is_read_from_the_recorded_answer);
    RUN_TEST(test_an_empty_listing_and_a_refusal_are_told_apart_from_damage);
    RUN_TEST(test_which_file_is_taken_and_which_items_are_skipped);
    RUN_TEST(test_kinds_and_addresses);
    RUN_TEST(test_cut_and_mutated_listings_do_no_harm);
    return UNITY_END();
}
