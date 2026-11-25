// SPDX-License-Identifier: Apache-2.0
// Makapix Club's answers read into records: the public promoted feed as
// recorded from the service, and the answer to a player's own question
// (written by hand from the server's schema until a registered player can
// record one), against what a Python reading of the same files expects
// (tests/assets/sources/make_expected.py); the questions p3a asks; where a
// file is; and the parser on cut and mutated input, for the sanitizers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "makapix_parse.h"
#include "p3a_err.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

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

static void digest_from_hex(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        unsigned byte;
        TEST_ASSERT_EQUAL(1, sscanf(hex + 2 * i, "%2x", &byte));
        out[i] = (uint8_t)byte;
    }
}

static uint32_t check_page(const char *listing, const char *expected_name, bool promoted)
{
    char *json = read_file(listing, NULL), *expected = read_file(expected_name, NULL);
    p3a_record_t *records = calloc(64, sizeof(p3a_record_t));
    static makapix_page_t page;
    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_page(json, promoted, records, 64, &page));
    char *line = strtok(expected, "\n");
    unsigned want_items, want_more;
    char want_cursor[MAKAPIX_CURSOR_LEN];
    TEST_ASSERT_EQUAL(3, sscanf(line, "%u %u %255s", &want_items, &want_more, want_cursor));
    TEST_ASSERT_EQUAL_UINT32(want_items, page.items);
    TEST_ASSERT_EQUAL(want_more != 0, page.has_more);
    TEST_ASSERT_EQUAL_STRING(strcmp(want_cursor, "-") == 0 ? "" : want_cursor, page.cursor);
    uint32_t at = 0;
    while ((line = strtok(NULL, "\n")) != NULL) {
        unsigned id, format, sort_at, modified_at, w, h;
        char key[64], digest_hex[80], url[160], got_key[MAKAPIX_UUID_TEXT], got_url[160];
        TEST_ASSERT_EQUAL_MESSAGE(9, sscanf(line, "%u %63s %u %u %u %u %u %79s %159s", &id, key, &format, &sort_at,
                                            &modified_at, &w, &h, digest_hex, url), line);
        TEST_ASSERT_TRUE_MESSAGE(at < page.count, "fewer records than expected");
        const p3a_record_t *r = &records[at++];
        TEST_ASSERT_EQUAL(P3A_SOURCE_MAKAPIX, r->source);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(id, r->id32, line);
        makapix_uuid_text(r->identity.makapix.uuid, got_key);
        TEST_ASSERT_EQUAL_STRING(key, got_key);
        TEST_ASSERT_EQUAL_MESSAGE(format, r->format, line);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sort_at, r->sort_at, line);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(modified_at, r->modified_at, line);
        TEST_ASSERT_EQUAL_MESSAGE(w, r->width, line);
        TEST_ASSERT_EQUAL_MESSAGE(h, r->height, line);
        TEST_ASSERT_FALSE(p3a_record_has_blob(r));
        TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(r));
        // Where the file is, from the digest the Python side computed of the key's text.
        uint8_t digest[32];
        digest_from_hex(digest_hex, digest);
        TEST_ASSERT_EQUAL(ESP_OK, makapix_vault_url(r, digest, got_url, sizeof(got_url)));
        TEST_ASSERT_EQUAL_STRING(url, got_url);
    }
    TEST_ASSERT_EQUAL_UINT32(at, page.count);
    const uint32_t count = page.count;
    free(records);
    free(json);
    free(expected);
    return count;
}

static void test_the_recorded_promoted_feed_becomes_records(void)
{
    TEST_ASSERT_EQUAL(8, check_page("makapix_promoted_p1.json", "makapix_promoted_p1.promoted.expected", true));
    TEST_ASSERT_EQUAL(8, check_page("makapix_promoted_p2.json", "makapix_promoted_p2.promoted.expected", true));
    // The same pages read as another channel's are ordered by listing, not promotion.
    TEST_ASSERT_EQUAL(8, check_page("makapix_promoted_p1.json", "makapix_promoted_p1.listed.expected", false));

    // The feed is newest promotion first: the order the channel plays in.
    char *json = read_file("makapix_promoted_p1.json", NULL);
    p3a_record_t *records = calloc(16, sizeof(p3a_record_t));
    static makapix_page_t page;
    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_page(json, true, records, 16, &page));
    for (uint32_t i = 1; i < page.count; i++) {
        TEST_ASSERT_TRUE(records[i - 1].sort_at >= records[i].sort_at);
        TEST_ASSERT_TRUE(records[i].sort_at > 1750000000u);
    }
    // No more room than three: three records, and the count of posts still told.
    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_page(json, true, records, 3, &page));
    TEST_ASSERT_EQUAL(3, page.count);
    TEST_ASSERT_EQUAL(8, page.items);
    free(records);
    free(json);
}

static void test_a_players_own_answer_becomes_records(void)
{
    // Seven posts: a GIF; a playlist (passed over); one whose address has no ending (its
    // native format decides) and whose times have "+00:00"; one with a storage key that is
    // not one; a promoted BMP; one in a format p3a does not know; one with no number.
    TEST_ASSERT_EQUAL(3, check_page("makapix_rpc_written.json", "makapix_rpc_written.listed.expected", false));
    TEST_ASSERT_EQUAL(3, check_page("makapix_rpc_written.json", "makapix_rpc_written.promoted.expected", true));
}

static esp_err_t parse(const char *json, makapix_page_t *page)
{
    static p3a_record_t records[4];
    return makapix_parse_page(json, false, records, 4, page);
}

static void test_refusals_and_other_answers_are_told_apart(void)
{
    static makapix_page_t page;
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING,
                      parse("{\"request_id\":\"x\",\"success\":false,\"error\":\"no\",\"error_code\":\"user_not_found\"}", &page));
    TEST_ASSERT_EQUAL(0, page.count);
    TEST_ASSERT_EQUAL(P3A_ERR_AUTH, parse("{\"success\":false,\"error_code\":\"authentication_failed\"}", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_RATE_LIMITED, parse("{\"success\":false,\"error_code\":\"rate_limited\"}", &page));
    TEST_ASSERT_EQUAL(ESP_FAIL, parse("{\"success\":false,\"error_code\":\"internal_error\"}", &page));
    TEST_ASSERT_EQUAL(ESP_FAIL, parse("{\"success\":false}", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, parse("", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, parse("[]", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, parse("{}", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, parse("{\"success\":true}", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, parse("{\"success\":true,\"posts\":7}", &page));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, parse("{\"detail\":\"Invalid or missing player token\",\"code\":\"unauthorized\"}", &page));
    // An empty channel is an answer.
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"success\":true,\"posts\":[],\"has_more\":false}", &page));
    TEST_ASSERT_EQUAL(0, page.items);
    TEST_ASSERT_FALSE(page.has_more);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"items\":[],\"next_cursor\":null}", &page));
    TEST_ASSERT_FALSE(page.has_more);
    // "More" needs a cursor to ask with, and a cursor that could not be sent back is none.
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"success\":true,\"posts\":[],\"has_more\":true}", &page));
    TEST_ASSERT_FALSE(page.has_more);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"success\":true,\"posts\":[],\"has_more\":true,\"next_cursor\":\"a\\\"b\"}", &page));
    TEST_ASSERT_FALSE(page.has_more);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"success\":true,\"posts\":[],\"has_more\":false,\"next_cursor\":\"50\"}", &page));
    TEST_ASSERT_FALSE(page.has_more);

    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_result("{\"request_id\":\"x\",\"success\":true,\"error\":null}"));
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, makapix_parse_result("{\"success\":false,\"error_code\":\"deleted\"}"));
    TEST_ASSERT_EQUAL(ESP_FAIL, makapix_parse_result("{\"success\":false,\"error_code\":\"reaction_limit_exceeded\"}"));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_result("{\"ok\":true}"));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_result("nonsense"));

    char token[32];
    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_token("{\"api_token\":\"mpx_live_abc-DEF_123\",\"rotated_at\":\"x\"}", token,
                                                 sizeof(token)));
    TEST_ASSERT_EQUAL_STRING("mpx_live_abc-DEF_123", token);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_token("{\"api_token\":null}", token, sizeof(token)));
    TEST_ASSERT_EQUAL_STRING("", token);
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_token("{\"api_token\":\"\"}", token, sizeof(token)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_token("{\"api_token\":\"a\\r\\nX-Other: b\"}", token, sizeof(token)));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_token("{\"api_token\":\"mpx_live_abc-DEF_123\"}", token, 8));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, makapix_parse_token("[]", token, sizeof(token)));
}

static void test_times_and_storage_keys_are_read_strictly(void)
{
    TEST_ASSERT_EQUAL_UINT32(946684800u, makapix_time("2000-01-01T00:00:00Z"));
    TEST_ASSERT_EQUAL_UINT32(1795536378u, makapix_time("2026-11-24T16:06:18.461787Z"));
    TEST_ASSERT_EQUAL_UINT32(1795536378u, makapix_time("2026-11-24T16:06:18+00:00"));
    TEST_ASSERT_EQUAL_UINT32(1795536378u, makapix_time("2026-11-24T16:06:18.4+00:00"));
    TEST_ASSERT_EQUAL_UINT32(1709164800u, makapix_time("2024-02-29T00:00:00Z"));  // a leap day
    TEST_ASSERT_EQUAL_UINT32(1709251199u, makapix_time("2024-02-29T23:59:59Z"));
    TEST_ASSERT_EQUAL_UINT32(1709251200u, makapix_time("2024-03-01T00:00:00Z"));
    TEST_ASSERT_EQUAL(0, makapix_time(NULL));
    TEST_ASSERT_EQUAL(0, makapix_time(""));
    TEST_ASSERT_EQUAL(0, makapix_time("2026-11-24"));
    TEST_ASSERT_EQUAL(0, makapix_time("2026-11-24T16:06:18"));        // no zone: not known to be UTC
    TEST_ASSERT_EQUAL(0, makapix_time("2026-11-24T16:06:18+01:00"));  // another zone
    TEST_ASSERT_EQUAL(0, makapix_time("2026-11-24T16:06:18.Z"));
    TEST_ASSERT_EQUAL(0, makapix_time("2026-13-24T16:06:18Z"));
    TEST_ASSERT_EQUAL(0, makapix_time("2026-11-24T25:06:18Z"));
    TEST_ASSERT_EQUAL(0, makapix_time("1970-01-01T00:00:00Z"));       // "the clock was not set"
    TEST_ASSERT_EQUAL(0, makapix_time("x026-11-24T16:06:18Z"));

    uint8_t uuid[16];
    char text[MAKAPIX_UUID_TEXT];
    TEST_ASSERT_TRUE(makapix_uuid_parse("F14EBF48-c4a3-449f-92c1-58a83224d984", uuid));
    TEST_ASSERT_EQUAL_HEX8(0xF1, uuid[0]);
    TEST_ASSERT_EQUAL_HEX8(0x84, uuid[15]);
    makapix_uuid_text(uuid, text);
    TEST_ASSERT_EQUAL_STRING("f14ebf48-c4a3-449f-92c1-58a83224d984", text);
    TEST_ASSERT_FALSE(makapix_uuid_parse(NULL, uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("", uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("f14ebf48c4a3449f92c158a83224d984", uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("f14ebf48-c4a3-449f-92c1-58a83224d98", uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("f14ebf48-c4a3-449f-92c1-58a83224d9844", uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("f14ebf48-c4a3-449f-92c1-58a83224d98g", uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("f14ebf48-c4a3-449f-92c1058a83224d984", uuid));
    TEST_ASSERT_FALSE(makapix_uuid_parse("f14ebf4-8c4a3-449f-92c1-58a83224d984", uuid));
}

static void test_channels_and_what_is_asked_of_them(void)
{
    makapix_channel_t channel;
    char body[512];
    TEST_ASSERT_TRUE(makapix_channel_parse("all", "", &channel));
    TEST_ASSERT_EQUAL(MAKAPIX_CHANNEL_ALL, channel);
    TEST_ASSERT_TRUE(makapix_channel_parse("promoted", NULL, &channel));
    TEST_ASSERT_TRUE(makapix_channel_parse("user", NULL, &channel));
    TEST_ASSERT_TRUE(makapix_channel_parse("by_user", "QVJ", &channel));
    TEST_ASSERT_EQUAL(MAKAPIX_CHANNEL_BY_USER, channel);
    TEST_ASSERT_TRUE(makapix_channel_parse("hashtag", "pixel_art-8", &channel));
    TEST_ASSERT_TRUE(makapix_channel_parse("reactions", "QVJ", &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse("by_user", "", &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse("hashtag", NULL, &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse("hashtag", "#art", &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse("hashtag", "a\",\"x\":\"y", &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse("artwork", NULL, &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse("cma:department", "Japanese Art", &channel));
    TEST_ASSERT_FALSE(makapix_channel_parse(NULL, NULL, &channel));
    TEST_ASSERT_NULL(makapix_channel_name(MAKAPIX_CHANNEL_COUNT));

    TEST_ASSERT_EQUAL(ESP_OK, makapix_query_body(MAKAPIX_CHANNEL_ALL, NULL, NULL, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("{\"request_type\":\"query_posts\",\"channel\":\"all\",\"sort\":\"server_order\",\"limit\":50,"
                             "\"include_fields\":[\"artwork_modified_at\",\"width\",\"height\"]}", body);
    TEST_ASSERT_EQUAL(ESP_OK, makapix_query_body(MAKAPIX_CHANNEL_HASHTAG, "cats", "100", body, sizeof(body)));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"channel\":\"hashtag\",\"hashtag\":\"cats\","));
    TEST_ASSERT_NOT_NULL(strstr(body, ",\"cursor\":\"100\","));
    TEST_ASSERT_EQUAL(ESP_OK, makapix_query_body(MAKAPIX_CHANNEL_REACTIONS, "QVJ", "eyJpZCI6=", body, sizeof(body)));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"channel\":\"reactions\",\"user_sqid\":\"QVJ\","));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"cursor\":\"eyJpZCI6=\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, makapix_query_body(MAKAPIX_CHANNEL_ALL, NULL, "1\",\"x\":\"", body, sizeof(body)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, makapix_query_body(MAKAPIX_CHANNEL_COUNT, NULL, NULL, body, sizeof(body)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_query_body(MAKAPIX_CHANNEL_ALL, NULL, NULL, body, 40));

    char url[512];
    TEST_ASSERT_EQUAL(ESP_OK, makapix_feed_url(NULL, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://makapix.club/api/v1/feed/promoted?limit=50&fields=id,kind,storage_key,art_url,"
                             "width,height,artwork_modified_at,promoted_at,listed_at,created_at", url);
    TEST_ASSERT_EQUAL(ESP_OK, makapix_feed_url("eyJ+/A==", url, sizeof(url)));
    TEST_ASSERT_NOT_NULL(strstr(url, "created_at&cursor=eyJ%2B%2FA%3D%3D"));
    TEST_ASSERT_EQUAL('\0', url[strlen(url)]);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, makapix_feed_url("a b", url, sizeof(url)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_feed_url("eyJ+/A==", url, 160));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_feed_url(NULL, url, 20));

    TEST_ASSERT_EQUAL(ESP_OK, makapix_reaction_body(4028, false, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("{\"request_type\":\"submit_reaction\",\"post_id\":4028,\"emoji\":\"\xF0\x9F\x91\x8D\"}", body);
    TEST_ASSERT_EQUAL(ESP_OK, makapix_reaction_body(4028, true, body, sizeof(body)));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"revoke_reaction\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, makapix_reaction_body(0, false, body, sizeof(body)));
}

static void test_a_file_with_no_known_format_has_no_address(void)
{
    static const uint8_t digest[32] = {0xFF, 0x40, 0x12};
    uint8_t uuid[16];
    char url[160];
    p3a_record_t r;
    TEST_ASSERT_TRUE(makapix_uuid_parse("f14ebf48-c4a3-449f-92c1-58a83224d984", uuid));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_makapix(&r, 4028, uuid));
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, makapix_vault_url(&r, digest, url, sizeof(url)));
    r.format = P3A_FORMAT_JPEG;  // Makapix has none
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, makapix_vault_url(&r, digest, url, sizeof(url)));
    r.format = P3A_FORMAT_GIF;
    TEST_ASSERT_EQUAL(ESP_OK, makapix_vault_url(&r, digest, url, sizeof(url)));
    // Only six bits of each of the first two bytes.
    TEST_ASSERT_EQUAL_STRING("https://vault.makapix.club/3f/00/f14ebf48-c4a3-449f-92c1-58a83224d984.gif", url);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_vault_url(&r, digest, url, 40));
    p3a_record_t giphy;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_giphy(&giphy, "abc"));
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, makapix_vault_url(&giphy, digest, url, sizeof(url)));
}

static void test_cut_and_mutated_answers_do_no_harm(void)
{
    static const char *const files[] = {"makapix_promoted_p1.json", "makapix_rpc_written.json"};
    p3a_record_t *records = calloc(16, sizeof(p3a_record_t));
    static makapix_page_t page;
    uint32_t seed = 4028;
    for (size_t f = 0; f < 2; f++) {
        size_t len = 0;
        char *json = read_file(files[f], &len);
        char *copy = malloc(len + 1);
        for (size_t cut = 0; cut < len; cut += 37) {
            memcpy(copy, json, cut);
            copy[cut] = '\0';
            makapix_parse_page(copy, f == 0, records, 16, &page);
            makapix_parse_result(copy);
        }
        for (int round = 0; round < 1500; round++) {
            memcpy(copy, json, len + 1);
            for (int k = 0; k < 1 + round % 6; k++) {
                seed = seed * 1664525u + 1013904223u;
                const size_t at = (seed >> 8) % len;
                seed = seed * 1664525u + 1013904223u;
                copy[at] = (char)(1 + (seed >> 16) % 255);
            }
            if (makapix_parse_page(copy, round % 2 == 0, records, 16, &page) == ESP_OK) {
                TEST_ASSERT_TRUE(page.count <= 16 && page.count <= page.items);
                TEST_ASSERT_TRUE(strlen(page.cursor) < MAKAPIX_CURSOR_LEN);
                for (uint32_t i = 0; i < page.count; i++) {
                    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&records[i]));
                }
            }
        }
        free(copy);
        free(json);
    }
    free(records);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_recorded_promoted_feed_becomes_records);
    RUN_TEST(test_a_players_own_answer_becomes_records);
    RUN_TEST(test_refusals_and_other_answers_are_told_apart);
    RUN_TEST(test_times_and_storage_keys_are_read_strictly);
    RUN_TEST(test_channels_and_what_is_asked_of_them);
    RUN_TEST(test_a_file_with_no_known_format_has_no_address);
    RUN_TEST(test_cut_and_mutated_answers_do_no_harm);
    return UNITY_END();
}
