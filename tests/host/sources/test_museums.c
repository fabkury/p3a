// SPDX-License-Identifier: Apache-2.0
// The museums' listings read into records, on responses recorded from each
// museum and against what a Python reading of the same files expects
// (tests/assets/sources/make_expected.py); the addresses p3a asks for; and
// every museum's parser on cut and mutated input, for the sanitizers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "museum.h"
#include "p3a_err.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define NOW  1700000000u
#define ROOM 128

static char *read_file(const char *name, const char *suffix, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s%s", P3A_SOURCE_ASSETS, name, suffix);
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

static p3a_record_t s_records[ROOM];

static museum_page_t fresh_page(void)
{
    museum_page_t page = {.out = s_records, .room = ROOM, .now = NOW};
    return page;
}

// Parses a recorded listing and compares it with its .expected file; returns how many records.
static uint32_t check(const museum_t *museum, const char *name)
{
    char *json = read_file(name, ".json", NULL), *expected = read_file(name, ".expected", NULL);
    museum_page_t page = fresh_page();
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, museum->parse_page(json, &page), name);
    char *line = strtok(expected, "\n");
    unsigned items, more;
    TEST_ASSERT_EQUAL(2, sscanf(line, "%u %u", &items, &more));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(items, page.items, name);
    TEST_ASSERT_EQUAL_MESSAGE(more != 0, page.more, name);
    uint32_t at = 0;
    while ((line = strtok(NULL, "\n")) != NULL) {
        char key[128], url[256];
        unsigned w, h;
        TEST_ASSERT_EQUAL(3, sscanf(line, "%127s %u %u", key, &w, &h));
        TEST_ASSERT_TRUE_MESSAGE(at < page.count, "fewer records than expected");
        const p3a_record_t *r = &s_records[at++];
        TEST_ASSERT_EQUAL_STRING(key, r->identity.museum.iiif_key);
        TEST_ASSERT_EQUAL(museum->number, r->identity.museum.museum_id);
        TEST_ASSERT_EQUAL_MESSAGE(w, r->width, line);
        TEST_ASSERT_EQUAL_MESSAGE(h, r->height, line);
        TEST_ASSERT_TRUE(r->width <= MUSEUM_LONGEST_SIDE && r->height <= MUSEUM_LONGEST_SIDE);
        TEST_ASSERT_EQUAL_UINT32(NOW, r->sort_at);
        TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(r));
        TEST_ASSERT_EQUAL_PTR(museum, museum_of_record(r));
        TEST_ASSERT_EQUAL(ESP_OK, museum->image_url(r, url, sizeof(url)));
        TEST_ASSERT_NOT_NULL(strstr(url, key));
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(at, page.count, name);
    free(json);
    free(expected);
    return page.count;
}

static void test_recorded_listings_are_read_as_an_independent_reading_expects(void)
{
    TEST_ASSERT_TRUE(check(&museum_vam, "vam_category_p1") >= 20);
    TEST_ASSERT_TRUE(check(&museum_vam, "vam_category_p3") >= 20);
    TEST_ASSERT_EQUAL(0, check(&museum_vam, "vam_empty"));
    TEST_ASSERT_TRUE(check(&museum_smk, "smk_collection_p1") >= 15);
    TEST_ASSERT_EQUAL(0, check(&museum_smk, "smk_empty"));
}

static void test_a_channels_kind_names_a_museum_and_one_of_its_axes(void)
{
    const char *axis = NULL;
    TEST_ASSERT_EQUAL_PTR(&museum_vam, museum_of_kind("vam:category", &axis));
    TEST_ASSERT_EQUAL_STRING("category", axis);
    TEST_ASSERT_EQUAL_PTR(&museum_vam, museum_of_kind("vam:venue", NULL));
    TEST_ASSERT_EQUAL_PTR(&museum_smk, museum_of_kind("smk:collection", &axis));
    TEST_ASSERT_NULL(museum_of_kind("vam:department", &axis));  // not one of its axes
    TEST_ASSERT_NULL(museum_of_kind("louvre:collection", &axis));
    TEST_ASSERT_NULL(museum_of_kind("vam", &axis));
    TEST_ASSERT_NULL(museum_of_kind("va:category", &axis));
    TEST_ASSERT_NULL(museum_of_kind("", &axis));
    TEST_ASSERT_NULL(museum_of_kind(NULL, &axis));
    // Every museum in the table: a number of its own, found again by its records.
    bool seen[MUSEUM_COUNT_] = {false};
    for (size_t i = 0; museum_at(i) != NULL; i++) {
        const museum_t *m = museum_at(i);
        TEST_ASSERT_TRUE(m->number < MUSEUM_COUNT_ && !seen[m->number]);
        seen[m->number] = true;
        TEST_ASSERT_TRUE(m->page_items > 0 && m->response_bytes >= 64 * 1024 && m->name[0] != '\0');
        p3a_record_t r;
        TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_museum(&r, m->number, "abc"));
        TEST_ASSERT_EQUAL_PTR(m, museum_of_record(&r));
    }
    p3a_record_t other;
    p3a_record_init_museum(&other, 200, "abc");  // a museum a later firmware knows
    TEST_ASSERT_NULL(museum_of_record(&other));
    p3a_record_init_giphy(&other, "abc");
    TEST_ASSERT_NULL(museum_of_record(&other));
}

static void test_the_addresses(void)
{
    char url[512];
    museum_query_t q = {.axis = "category", .term = "THES48903", .api_key = "", .start = 0, .next = ""};
    TEST_ASSERT_EQUAL(ESP_OK, museum_vam.list_url(&q, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.vam.ac.uk/v2/objects/search?page=1&page_size=100&images_exist=1&id_category=THES48903",
                             url);
    q.axis = "venue";
    q.term = "a b&c=d";
    q.start = 300;
    TEST_ASSERT_EQUAL(ESP_OK, museum_vam.list_url(&q, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.vam.ac.uk/v2/objects/search?page=4&page_size=100&images_exist=1&id_venue=a%20b%26c%3Dd",
                             url);
    q.axis = "department";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, museum_vam.list_url(&q, url, sizeof(url)));
    q.axis = "category";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, museum_vam.list_url(&q, url, 60));

    q = (museum_query_t){.axis = "collection", .term = "Gammel bestand", .api_key = "", .start = 150, .next = ""};
    TEST_ASSERT_EQUAL(ESP_OK, museum_smk.list_url(&q, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.smk.dk/api/v1/art/search?keys=*&offset=150&rows=50"
                             "&filters=%5Bcollection%3AGammel%20bestand%5D%2C%5Bhas_image%3Atrue%5D",
                             url);
    q.term = "x],[has_image:false";  // a term cannot close its bracket and open another filter
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, museum_smk.list_url(&q, url, sizeof(url)));
    q.term = "";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, museum_smk.list_url(&q, url, sizeof(url)));

    p3a_record_t r;
    p3a_record_init_museum(&r, MUSEUM_VAM, "2016JL6674");
    TEST_ASSERT_EQUAL(ESP_OK, museum_vam.image_url(&r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://framemark.vam.ac.uk/collections/2016JL6674/full/!720,720/0/default.jpg", url);
    p3a_record_init_museum(&r, MUSEUM_SMK, "bc386p50w_kksgb22235.tif.jp2");
    TEST_ASSERT_EQUAL(ESP_OK, museum_smk.image_url(&r, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://iip.smk.dk/iiif/jp2/bc386p50w_kksgb22235.tif.jp2/full/!720,720/0/default.jpg", url);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, museum_smk.image_url(&r, url, 40));
    // A key from a damaged index is not made into an address.
    p3a_record_init_museum(&r, MUSEUM_SMK, "a/../../b");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, museum_smk.image_url(&r, url, sizeof(url)));
}

static void test_keys_and_sizes(void)
{
    TEST_ASSERT_TRUE(museum_key_ok("2016JL6674"));
    TEST_ASSERT_TRUE(museum_key_ok("bc386p50w_kksgb22235.tif.jp2"));
    static const char *const bad[] = {NULL, "", ".", "..", "a/b", "a b", "a?b", "a#b", "a%2Fb", "\xC3\xA9",
                                      "0123456789012345678901234567890123456789012345678901234567890123456789"
                                      "01234567890123456789012345"};  // 96 characters: one too many
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(museum_key_ok(bad[i]), bad[i] ? bad[i] : "NULL");
    }
    museum_page_t page = fresh_page();
    museum_page_add(&page, MUSEUM_SMK, "tall", 3000, 6000);
    museum_page_add(&page, MUSEUM_SMK, "small", 300, 200);       // never enlarged
    museum_page_add(&page, MUSEUM_SMK, "unknown", 0, 500);
    museum_page_add(&page, MUSEUM_SMK, "sliver", 90000, 10);      // one pixel high at least
    museum_page_add(&page, MUSEUM_SMK, "absurd", 4000000000u, 5);
    museum_page_add(&page, MUSEUM_SMK, "bad key", 10, 10);
    TEST_ASSERT_EQUAL(5, page.count);
    TEST_ASSERT_EQUAL(360, s_records[0].width);
    TEST_ASSERT_EQUAL(720, s_records[0].height);
    TEST_ASSERT_EQUAL(300, s_records[1].width);
    TEST_ASSERT_EQUAL(0, s_records[2].width);
    TEST_ASSERT_EQUAL(0, s_records[2].height);
    TEST_ASSERT_EQUAL(720, s_records[3].width);
    TEST_ASSERT_EQUAL(1, s_records[3].height);
    TEST_ASSERT_EQUAL(0, s_records[4].width);
    // No more than there is room for.
    page = fresh_page();
    page.room = 2;
    for (int i = 0; i < 5; i++) {
        museum_page_add(&page, MUSEUM_VAM, i == 0 ? "a" : i == 1 ? "b" : "c", 0, 0);
    }
    TEST_ASSERT_EQUAL(2, page.count);
}

static void test_what_is_not_a_listing_is_refused(void)
{
    static const char *const not_listings[] = {"", "<html>503</html>", "[]", "{}", "{\"records\":{}}", "{\"items\":7}",
                                               "null"};
    for (size_t m = 0; museum_at(m) != NULL; m++) {
        for (size_t i = 0; i < sizeof(not_listings) / sizeof(not_listings[0]); i++) {
            museum_page_t page = fresh_page();
            TEST_ASSERT_EQUAL_MESSAGE(P3A_ERR_CORRUPT, museum_at(m)->parse_page(not_listings[i], &page), museum_at(m)->id);
            TEST_ASSERT_EQUAL(0, page.count);
        }
    }
    // Items that cannot be used are counted and skipped.
    museum_page_t page = fresh_page();
    TEST_ASSERT_EQUAL(ESP_OK, museum_vam.parse_page("{\"records\":[{\"_primaryImageId\":\"ok1\"},{\"_primaryImageId\":\"\"},"
                                                    "{\"_primaryImageId\":7},{},3,{\"_primaryImageId\":\"a/b\"}],"
                                                    "\"info\":{\"page\":2,\"pages\":2}}", &page));
    TEST_ASSERT_EQUAL(6, page.items);
    TEST_ASSERT_EQUAL(1, page.count);
    TEST_ASSERT_FALSE(page.more);
    page = fresh_page();
    TEST_ASSERT_EQUAL(ESP_OK, museum_smk.parse_page(
        "{\"offset\":50,\"found\":53,\"items\":["
        "{\"image_iiif_id\":\"https://iip.smk.dk/iiif/jp2/one.jp2\",\"image_width\":1440,\"image_height\":720},"
        "{\"image_iiif_id\":\"https://x/iiif/jp2/y/iiif/jp2/two.jp2\"},"
        "{\"image_iiif_id\":\"https://elsewhere/no-marker.jp2\"},{\"image_thumbnail\":\"t\"}]}", &page));
    TEST_ASSERT_EQUAL(4, page.items);
    TEST_ASSERT_EQUAL(2, page.count);
    TEST_ASSERT_EQUAL_STRING("one.jp2", s_records[0].identity.museum.iiif_key);
    TEST_ASSERT_EQUAL(720, s_records[0].width);
    TEST_ASSERT_EQUAL(360, s_records[0].height);
    TEST_ASSERT_EQUAL_STRING("two.jp2", s_records[1].identity.museum.iiif_key);  // after the last marker
    TEST_ASSERT_FALSE(page.more);  // 50 + 4 is past the 53 found
}

static void mutate(const museum_t *museum, const char *name, int rounds)
{
    size_t len = 0;
    char *json = read_file(name, ".json", &len);
    char *copy = malloc(len + 1);
    uint32_t seed = 4242;
    for (size_t cut = 0; cut < len; cut += 211) {
        memcpy(copy, json, cut);
        copy[cut] = '\0';
        museum_page_t page = fresh_page();
        museum->parse_page(copy, &page);
    }
    for (int round = 0; round < rounds; round++) {
        memcpy(copy, json, len + 1);
        for (int k = 0; k < 1 + round % 6; k++) {
            seed = seed * 1664525u + 1013904223u;
            const size_t at = (seed >> 8) % len;
            seed = seed * 1664525u + 1013904223u;
            copy[at] = (char)(1 + (seed >> 16) % 255);
        }
        museum_page_t page = fresh_page();
        if (museum->parse_page(copy, &page) == ESP_OK) {
            TEST_ASSERT_TRUE(page.count <= page.items && page.count <= ROOM);
            for (uint32_t i = 0; i < page.count; i++) {
                char url[256];
                TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&s_records[i]));
                TEST_ASSERT_EQUAL(ESP_OK, museum->image_url(&s_records[i], url, sizeof(url)));
                TEST_ASSERT_EQUAL(0, strncmp(url, "https://", 8));
            }
        }
    }
    free(copy);
    free(json);
}

static void test_cut_and_mutated_listings_do_no_harm(void)
{
    mutate(&museum_vam, "vam_category_p1", 1500);
    mutate(&museum_smk, "smk_collection_p1", 800);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_recorded_listings_are_read_as_an_independent_reading_expects);
    RUN_TEST(test_a_channels_kind_names_a_museum_and_one_of_its_axes);
    RUN_TEST(test_the_addresses);
    RUN_TEST(test_keys_and_sizes);
    RUN_TEST(test_what_is_not_a_listing_is_refused);
    RUN_TEST(test_cut_and_mutated_listings_do_no_harm);
    return UNITY_END();
}
