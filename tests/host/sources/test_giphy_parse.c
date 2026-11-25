// SPDX-License-Identifier: Apache-2.0
// Giphy's listing read into records, on responses recorded from the service
// and against what a Python reading of the same files expects
// (tests/assets/sources/make_expected.py); the addresses p3a asks for; and
// the parser on cut and mutated input, for the sanitizers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "giphy_parse.h"
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

static const giphy_parse_opts_t DOWNSIZED = {.file = GIPHY_FILE_200_GIF, .prefer_downsized = true, .screen_w = 720,
                                             .screen_h = 720, .now = NOW};
static const giphy_parse_opts_t FIXED = {.file = GIPHY_FILE_200_GIF, .screen_w = 720, .screen_h = 720, .now = NOW};

static void check_against(const char *listing, const char *expected_name, const giphy_parse_opts_t *opts)
{
    char *json = read_file(listing, NULL), *expected = read_file(expected_name, NULL);
    p3a_record_t *records = calloc(64, sizeof(p3a_record_t));
    uint32_t count = 0, items = 0;
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_page(json, opts, records, 64, &count, &items));
    char *line = strtok(expected, "\n");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)atoi(line), items);
    uint32_t at = 0;
    while ((line = strtok(NULL, "\n")) != NULL) {
        char id[64];
        unsigned file, w, h, sort_at;
        TEST_ASSERT_EQUAL(5, sscanf(line, "%63s %u %u %u %u", id, &file, &w, &h, &sort_at));
        TEST_ASSERT_TRUE_MESSAGE(at < count, "fewer records than expected");
        const p3a_record_t *r = &records[at++];
        TEST_ASSERT_EQUAL_STRING(id, r->identity.giphy.id);
        TEST_ASSERT_EQUAL_MESSAGE(file, r->identity.giphy.file, id);
        TEST_ASSERT_EQUAL_MESSAGE(w, r->width, id);
        TEST_ASSERT_EQUAL_MESSAGE(h, r->height, id);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sort_at, r->sort_at, id);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, p3a_record_check(r), id);
        TEST_ASSERT_FALSE(p3a_record_has_blob(r));
    }
    TEST_ASSERT_EQUAL_UINT32(at, count);
    TEST_ASSERT_TRUE_MESSAGE(count >= 20, "the recorded page holds a real listing");
    free(records);
    free(json);
    free(expected);
}

static void test_recorded_listings_are_read_as_an_independent_reading_expects(void)
{
    check_against("giphy_trending.json", "giphy_trending.downsized.expected", &DOWNSIZED);
    check_against("giphy_trending.json", "giphy_trending.fixed.expected", &FIXED);
    check_against("giphy_search.json", "giphy_search.downsized.expected", &DOWNSIZED);
    check_against("giphy_search.json", "giphy_search.fixed.expected", &FIXED);
}

static void test_an_empty_listing_and_a_refusal_are_told_apart_from_damage(void)
{
    p3a_record_t records[4];
    uint32_t count = 9, items = 9;
    char *empty = read_file("giphy_search_empty.json", NULL);
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_page(empty, &FIXED, records, 4, &count, &items));
    TEST_ASSERT_EQUAL(0, count);
    TEST_ASSERT_EQUAL(0, items);
    free(empty);
    // Giphy's answer to a bad key has "data": [] too; the fetcher reports the 401 before this is read.
    char *refused = read_file("giphy_bad_key.json", NULL);
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_page(refused, &FIXED, records, 4, &count, &items));
    TEST_ASSERT_EQUAL(0, items);
    free(refused);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_page("", &FIXED, records, 4, &count, &items));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_page("<html>503</html>", &FIXED, records, 4, &count, &items));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_page("{\"data\":{}}", &FIXED, records, 4, &count, &items));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_page("[1,2]", &FIXED, records, 4, &count, &items));
}

static void test_items_that_cannot_be_used_are_skipped_but_counted(void)
{
    static const char *json =
        "{\"data\":["
        "{\"id\":\"good1\",\"images\":{\"fixed_height\":{\"width\":\"356\",\"height\":\"200\"}}},"
        "{\"id\":\"\"},"
        "{\"id\":42},"
        "{\"id\":\"../../etc\"},"
        "{\"id\":\"has space\"},"
        "{\"id\":\"0123456789012345678901234567890123456789\"},"
        "\"text\","
        "{\"id\":\"good2\",\"images\":\"none\",\"trending_datetime\":\"0000-00-00 00:00:00\","
        "\"import_datetime\":\"2020-02-29 12:00:00\"},"
        "{\"id\":\"good3\",\"images\":{\"fixed_height\":{\"width\":99999,\"height\":-4},"
        "\"downsized_medium\":{\"width\":\"2000\",\"height\":\"300\",\"url\":\"https://x/giphy.gif\"}}},"
        "{\"id\":\"good4\",\"images\":{\"downsized_medium\":{\"width\":\"1080\",\"height\":\"1080\","
        "\"url\":\"https://media.giphy.com/media/v1.abc/good4/giphy.gif?cid=1&rid=giphy.gif\"}}},"
        "{\"id\":\"good5\",\"images\":{\"downsized_medium\":{\"width\":\"400\",\"height\":\"400\","
        "\"url\":\"https://x/some-other-file.gif\"}}}"
        "]}";
    p3a_record_t records[8];
    uint32_t count = 0, items = 0;
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_page(json, &DOWNSIZED, records, 8, &count, &items));
    TEST_ASSERT_EQUAL(11, items);
    TEST_ASSERT_EQUAL(5, count);
    TEST_ASSERT_EQUAL_STRING("good1", records[0].identity.giphy.id);
    TEST_ASSERT_EQUAL(356, records[0].width);
    TEST_ASSERT_EQUAL(NOW, records[0].sort_at);  // no date: the time of the listing
    TEST_ASSERT_EQUAL_STRING("good2", records[1].identity.giphy.id);
    TEST_ASSERT_EQUAL(1582977600u, records[1].sort_at);  // the import date, when it never trended
    TEST_ASSERT_EQUAL(0, records[1].width);
    TEST_ASSERT_EQUAL(GIPHY_FILE_200_GIF, records[2].identity.giphy.file);  // 2000 wide: more than a screen and a half
    TEST_ASSERT_EQUAL(0, records[2].width);                                 // and sizes out of range are not believed
    TEST_ASSERT_EQUAL(GIPHY_FILE_ORIGINAL_GIF, records[3].identity.giphy.file);  // 1080: exactly a screen and a half
    TEST_ASSERT_EQUAL(1080, records[3].width);
    TEST_ASSERT_EQUAL(GIPHY_FILE_200_GIF, records[4].identity.giphy.file);  // a file p3a has no name for

    // No more records than there is room for; the page is still counted whole.
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_page(json, &DOWNSIZED, records, 2, &count, &items));
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(11, items);
}

static void test_dates_are_utc_and_anything_else_is_no_date(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, giphy_parse_datetime("1970-01-01 00:00:00"));
    TEST_ASSERT_EQUAL_UINT32(1, giphy_parse_datetime("1970-01-01 00:00:01"));
    TEST_ASSERT_EQUAL_UINT32(951782400u, giphy_parse_datetime("2000-02-29 00:00:00"));
    TEST_ASSERT_EQUAL_UINT32(1795529120u, giphy_parse_datetime("2026-11-24 14:05:20"));
    TEST_ASSERT_EQUAL_UINT32(4102444799u, giphy_parse_datetime("2099-12-31 23:59:59"));
    static const char *const none[] = {NULL, "", "0000-00-00 00:00:00", "2026-11-24", "2026-11-24T14:05:20",
                                       "2026-13-01 00:00:00", "2026-00-10 00:00:00", "2026-01-32 00:00:00",
                                       "2026-01-01 24:00:00", "1969-12-31 23:59:59", "9999-01-01 00:00:00",
                                       "2026-1x-01 00:00:00", "2026-11-24 14:05:20 "};
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, giphy_parse_datetime(none[i]), none[i] ? none[i] : "NULL");
    }
}

static void test_the_download_address_follows_the_file_in_the_record(void)
{
    p3a_record_t r;
    char url[96];
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_giphy(&r, "3o7TKSjRrfIPjeiVyM"));
    TEST_ASSERT_EQUAL(ESP_OK, giphy_download_url(&r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://i.giphy.com/media/3o7TKSjRrfIPjeiVyM/200.gif", url);
    static const char *const names[GIPHY_FILE_END_] = {"200.gif", "200.gif", "200.webp", "200w.gif", "200w.webp",
                                                       "giphy.gif", "giphy.webp", "giphy-downsized-medium.gif",
                                                       "giphy-downsized.gif"};
    for (uint8_t file = 0; file < GIPHY_FILE_END_; file++) {
        r.identity.giphy.file = file;
        TEST_ASSERT_EQUAL(ESP_OK, giphy_download_url(&r, url, sizeof(url)));
        TEST_ASSERT_EQUAL_STRING(names[file], strrchr(url, '/') + 1);
    }
    r.identity.giphy.file = 200;  // a number a later firmware wrote
    TEST_ASSERT_EQUAL(ESP_OK, giphy_download_url(&r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("200.gif", strrchr(url, '/') + 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, giphy_download_url(&r, url, 20));
    // Not a Giphy record, and an id that is not one: no address is made of them.
    p3a_record_t local;
    p3a_record_init_local(&local, "a.gif");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, giphy_download_url(&local, url, sizeof(url)));
    p3a_record_init_giphy(&r, "a/../b");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, giphy_download_url(&r, url, sizeof(url)));

    TEST_ASSERT_EQUAL(GIPHY_FILE_200_GIF, giphy_file_for("fixed_height", false));
    TEST_ASSERT_EQUAL(GIPHY_FILE_200W_WEBP, giphy_file_for("fixed_width", true));
    TEST_ASSERT_EQUAL(GIPHY_FILE_ORIGINAL_GIF, giphy_file_for("original", false));
    TEST_ASSERT_EQUAL(GIPHY_FILE_DOWNSIZED_MEDIUM, giphy_file_for("downsized_medium", true));  // GIF only
    TEST_ASSERT_EQUAL(GIPHY_FILE_200_WEBP, giphy_file_for("no such rendition", true));
    TEST_ASSERT_EQUAL(GIPHY_FILE_200_GIF, giphy_file_for(NULL, false));
}

static void test_the_listing_address_carries_what_was_asked_and_encodes_it(void)
{
    char url[512];
    giphy_list_request_t r = {.api_key = "KEY123", .rating = "pg-13", .country = "", .random_id = "",
                              .file = GIPHY_FILE_200_GIF, .prefer_downsized = true, .offset = 0};
    TEST_ASSERT_EQUAL(ESP_OK, giphy_list_url(&r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.giphy.com/v1/gifs/trending?api_key=KEY123&limit=50&offset=0&rating=pg-13"
                             "&fields=id,trending_datetime,import_datetime,images.fixed_height,images.downsized_medium",
                             url);
    r = (giphy_list_request_t){.api_key = "KEY123", .query = "pixel art & \"cats\" \xC3\xA9", .rating = "g",
                               .country = "BR", .random_id = "abc 1", .file = GIPHY_FILE_ORIGINAL_WEBP, .offset = 150};
    TEST_ASSERT_EQUAL(ESP_OK, giphy_list_url(&r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.giphy.com/v1/gifs/search?api_key=KEY123&q=pixel%20art%20%26%20%22cats%22%20%C3%A9"
                             "&limit=50&offset=150&rating=g&fields=id,trending_datetime,import_datetime,images.original"
                             "&random_id=abc%201&country_code=BR",
                             url);
    // Too long for the buffer is an error, never a cut address.
    char small[64];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, giphy_list_url(&r, small, sizeof(small)));
    r.api_key = "";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, giphy_list_url(&r, url, sizeof(url)));
    // A key is whatever a person typed: it cannot add a parameter of its own.
    TEST_ASSERT_EQUAL(ESP_OK, giphy_random_id_url("a&b=c d", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.giphy.com/v1/randomid?api_key=a%26b%3Dc%20d", url);
}

static void test_the_two_small_answers(void)
{
    char out[128];
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_random_id("{\"data\":{\"random_id\":\"e826c9fc5c929e0d6c6d423841a282aa\"},"
                                                    "\"meta\":{\"status\":200}}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("e826c9fc5c929e0d6c6d423841a282aa", out);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_random_id("{\"data\":[]}", out, sizeof(out)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_random_id("{\"data\":{\"random_id\":\"\"}}", out, sizeof(out)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_random_id("nonsense", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, giphy_parse_random_id("{\"data\":{\"random_id\":\"0123456789\"}}", out, 8));
    // A click: where to ask, and what is made of the answer.
    p3a_record_t r;
    p3a_record_init_giphy(&r, "3o7TKSjRrfIPjeiVyM");
    TEST_ASSERT_EQUAL(ESP_OK, giphy_click_lookup_url("KEY1", &r, "rid 9", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://api.giphy.com/v1/gifs/3o7TKSjRrfIPjeiVyM?api_key=KEY1&fields=id,analytics&random_id=rid%209",
                             out);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, giphy_click_lookup_url("KEY1", &r, "", out, sizeof(out)));  // no id: no click
#define CLICK(url) "{\"data\":{\"id\":\"x\",\"analytics\":{\"onclick\":{\"url\":\"" url "\"}}}}"
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_click_url(CLICK("https://giphy-analytics.giphy.com/v2/pingback_simple?a=1"),
                                                    "rid 9", 1795529120123ull, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://giphy-analytics.giphy.com/v2/pingback_simple?a=1&random_id=rid%209&ts=1795529120123",
                             out);
    TEST_ASSERT_EQUAL(ESP_OK, giphy_parse_click_url(CLICK("https://a.giphy.com/p"), "r", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://a.giphy.com/p?random_id=r&ts=5", out);
    // An answer is not trusted to send the device anywhere but Giphy, or off https.
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_click_url(CLICK("https://example.org/x?a=1"), "r", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_click_url(CLICK("https://giphy.com.example.org/x"), "r", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_click_url(CLICK("https://a.giphy.com@example.org/x"), "r", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_click_url(CLICK("http://a.giphy.com/x"), "r", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, giphy_parse_click_url("{\"data\":{\"id\":\"x\"}}", "r", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, giphy_parse_click_url(CLICK("https://a.giphy.com/p"), "r", 5, out, 30));
}

// Every cut of a recorded page, and thousands of copies with bytes changed:
// nothing to assert but that every record made passes its check. The
// sanitizers do the rest.
static void test_cut_and_mutated_listings_do_no_harm(void)
{
    size_t len = 0;
    char *json = read_file("giphy_trending.json", &len);
    char *copy = malloc(len + 1);
    p3a_record_t *records = calloc(64, sizeof(p3a_record_t));
    uint32_t count, items, seed = 12345;
    for (size_t cut = 0; cut < len; cut += 97) {
        memcpy(copy, json, cut);
        copy[cut] = '\0';
        giphy_parse_page(copy, &DOWNSIZED, records, 64, &count, &items);
    }
    for (int round = 0; round < 3000; round++) {
        memcpy(copy, json, len + 1);
        for (int k = 0; k < 1 + round % 6; k++) {
            seed = seed * 1664525u + 1013904223u;
            const size_t at = (seed >> 8) % len;
            seed = seed * 1664525u + 1013904223u;
            copy[at] = (char)(1 + (seed >> 16) % 255);  // never a terminator: the text keeps its length
        }
        if (giphy_parse_page(copy, &DOWNSIZED, records, 64, &count, &items) == ESP_OK) {
            TEST_ASSERT_TRUE(count <= items && count <= 64);
            for (uint32_t i = 0; i < count; i++) {
                char url[96];
                TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&records[i]));
                TEST_ASSERT_EQUAL(ESP_OK, giphy_download_url(&records[i], url, sizeof(url)));
            }
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
    RUN_TEST(test_an_empty_listing_and_a_refusal_are_told_apart_from_damage);
    RUN_TEST(test_items_that_cannot_be_used_are_skipped_but_counted);
    RUN_TEST(test_dates_are_utc_and_anything_else_is_no_date);
    RUN_TEST(test_the_download_address_follows_the_file_in_the_record);
    RUN_TEST(test_the_listing_address_carries_what_was_asked_and_encodes_it);
    RUN_TEST(test_the_two_small_answers);
    RUN_TEST(test_cut_and_mutated_listings_do_no_harm);
    return UNITY_END();
}
