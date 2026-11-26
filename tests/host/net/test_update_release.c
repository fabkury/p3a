// SPDX-License-Identifier: Apache-2.0
// Which release a device is offered: on GitHub's answer as recorded while
// only 1.x had shipped, on that answer as it will look with 2.x beside it
// (tests/assets/update/make_releases.py), on lists made here, and on cut
// and mutated copies for the sanitizers. And the order of versions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "unity.h"
#include "update_release.h"

void setUp(void) {}
void tearDown(void) {}

#define PREFIX "https://github.com/fabkury/p3a/releases/download/"
#define SLOT   (9u * 1024u * 1024u)

static const update_want_t STABLE = {.prereleases = false, .max_bytes = SLOT, .url_prefix = PREFIX};
static const update_want_t ANY = {.prereleases = true, .max_bytes = SLOT, .url_prefix = PREFIX};

static char *read_file(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", P3A_UPDATE_ASSETS, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    TEST_ASSERT_EQUAL((size_t)size, fread(text, 1, (size_t)size, f));
    text[size] = '\0';
    fclose(f);
    *len = (size_t)size;
    return text;
}

static void hex(const uint8_t sha[32], char out[65])
{
    for (int i = 0; i < 32; i++) {
        snprintf(out + 2 * i, 3, "%02x", sha[i]);
    }
}

static void test_versions_are_ordered_as_semantic_versioning_orders_them(void)
{
    // Each is lower than the next (the list is the specification's own, with p3a's in front).
    static const char *const ordered[] = {
        "1.2.5",       "2.0.0-dev",       "2.0.0-rc.1",   "2.0.0-rc.2",   "2.0.0-rc.10", "2.0.0",  "2.0.1",
        "2.0.10",      "2.1.0-alpha",     "2.1.0-alpha.1", "2.1.0-alpha.beta", "2.1.0-beta", "2.1.0-beta.2",
        "2.1.0-beta.11", "2.1.0-rc.1",    "2.1.0",        "2.10.0",       "10.0.0",
    };
    const int n = (int)(sizeof(ordered) / sizeof(ordered[0]));
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(update_version_ok(ordered[i]), ordered[i]);
        for (int j = 0; j < n; j++) {
            const int c = update_version_compare(ordered[i], ordered[j]);
            TEST_ASSERT_EQUAL_MESSAGE(i < j ? -1 : i > j ? 1 : 0, c, ordered[i]);
        }
    }
    // Build metadata does not order; numbers are numbers.
    TEST_ASSERT_EQUAL(0, update_version_compare("2.0.0+abc", "2.0.0+xyz.1"));
    TEST_ASSERT_EQUAL(0, update_version_compare("2.0.0-rc.1+a", "2.0.0-rc.1"));
    TEST_ASSERT_EQUAL(0, update_version_compare("2.0.0-rc.01", "2.0.0-rc.1"));
    TEST_ASSERT_EQUAL(0, update_version_compare("02.0.0", "2.0.0"));
    // A number comes before a word.
    TEST_ASSERT_EQUAL(-1, update_version_compare("2.0.0-1", "2.0.0-a"));
}

static void test_what_is_not_a_version_is_refused_and_sorts_first(void)
{
    static const char *const bad[] = {
        "",        "v2.0.0",  "2.0",       "2",         "2.0.0.0",  "2.0.x",        "2.0.0-",   "2.0.0-a..b",
        "2.0.0-a.", "2.0.0+",  "2.0.0-a b", "2.0.0 ",    " 2.0.0",   "2.0.0-\xc3\xa9", "-1.0.0",  "2.-1.0",
        "1234567890.0.0",     "2.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",  "2..0",     "latest",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(update_version_ok(bad[i]), bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(-1, update_version_compare(bad[i], "0.0.0-0"), bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(1, update_version_compare("0.0.0-0", bad[i]), bad[i]);
    }
    TEST_ASSERT_FALSE(update_version_ok(NULL));
    TEST_ASSERT_EQUAL(0, update_version_compare("latest", NULL));
    TEST_ASSERT_TRUE(update_version_ok("999999999.0.0"));
    TEST_ASSERT_TRUE(update_version_ok("2.0.0-dev"));
}

static void test_the_list_recorded_while_only_1x_had_shipped_has_nothing_for_a_2x_device(void)
{
    size_t len = 0;
    char *json = read_file("github_releases_1x.json", &len);
    update_release_t release;
    int seen = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, update_release_pick(json, len, &ANY, &release, &seen));
    TEST_ASSERT_EQUAL(3, seen);
    free(json);
}

static void test_the_highest_version_with_a_whole_image_is_offered(void)
{
    size_t len = 0;
    char *json = read_file("github_releases_2x_made.json", &len);
    update_release_t release;
    char digest[65];
    int seen = 0;

    TEST_ASSERT_EQUAL(ESP_OK, update_release_pick(json, len, &STABLE, &release, &seen));
    TEST_ASSERT_EQUAL(5, seen);
    TEST_ASSERT_EQUAL_STRING("2.0.1", release.version);
    TEST_ASSERT_FALSE(release.prerelease);
    TEST_ASSERT_EQUAL_STRING(PREFIX "v2.0.1/p3a-v2.bin", release.url);
    TEST_ASSERT_EQUAL_UINT32(2453520, release.size);
    hex(release.sha256, digest);
    TEST_ASSERT_EQUAL_STRING("7815a66a3d192693a1fd72fa5b21838de40b65380909a0bca8dcf30276394bab", digest);
    TEST_ASSERT_EQUAL_STRING("## What's new\n\n- Fixes \xe2\x80\x94 caf\xc3\xa9 \xe2\x98\x95\n- More", release.notes);
    TEST_ASSERT_EQUAL(20, strlen(release.published));
    TEST_ASSERT_EQUAL('Z', release.published[19]);

    // With pre-releases wanted, the candidate is the highest.
    TEST_ASSERT_EQUAL(ESP_OK, update_release_pick(json, len, &ANY, &release, NULL));
    TEST_ASSERT_EQUAL_STRING("2.1.0-rc.1", release.version);
    TEST_ASSERT_TRUE(release.prerelease);
    TEST_ASSERT_EQUAL_UINT32(2461648, release.size);
    hex(release.sha256, digest);
    TEST_ASSERT_EQUAL_STRING("6e66d45b3168e8711b8be1e462a91a16b0944d192e74e0e7b68d6e16fd7a37cd", digest);

    // An image larger than the slot is not offered: the next one down is.
    const update_want_t small_slot = {.prereleases = true, .max_bytes = 2461647, .url_prefix = PREFIX};
    TEST_ASSERT_EQUAL(ESP_OK, update_release_pick(json, len, &small_slot, &release, NULL));
    TEST_ASSERT_EQUAL_STRING("2.0.1", release.version);
    // An address that is not where releases are kept is not offered.
    const update_want_t elsewhere = {.prereleases = true, .max_bytes = SLOT, .url_prefix = "https://example.com/"};
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, update_release_pick(json, len, &elsewhere, &release, NULL));
    free(json);
}

// One release with one asset, every part of it given.
static const char *one(const char *tag, const char *flags, const char *name, const char *state, const char *size,
                       const char *digest, const char *url)
{
    static char text[2048];
    snprintf(text, sizeof(text),
             "[{\"tag_name\":%s%s,\"published_at\":\"2027-01-02T03:04:05Z\",\"body\":\"notes\","
             "\"assets\":[{\"name\":\"other.bin\",\"size\":1},"
             "{\"name\":%s,\"state\":%s,\"size\":%s,\"digest\":%s,\"browser_download_url\":%s}]}]",
             tag, flags, name, state, size, digest, url);
    return text;
}

#define GOOD_DIGEST "\"sha256:00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\""
#define GOOD_URL    "\"" PREFIX "v2.3.4/p3a-v2.bin\""

static esp_err_t pick(const char *json, const update_want_t *want, update_release_t *out)
{
    return update_release_pick(json, strlen(json), want, out, NULL);
}

static void test_a_release_is_passed_over_for_each_thing_that_makes_it_unusable(void)
{
    update_release_t r;
    const char *good = one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL);
    TEST_ASSERT_EQUAL(ESP_OK, pick(good, &STABLE, &r));
    TEST_ASSERT_EQUAL_STRING("2.3.4", r.version);
    TEST_ASSERT_EQUAL_UINT32(3000000, r.size);
    TEST_ASSERT_EQUAL_HEX8(0x00, r.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, r.sha256[15]);
    TEST_ASSERT_EQUAL_HEX8(0xff, r.sha256[31]);
    TEST_ASSERT_EQUAL_STRING("2027-01-02T03:04:05Z", r.published);
    TEST_ASSERT_EQUAL_STRING("notes", r.notes);
    // A tag without the "v" is a version too.
    TEST_ASSERT_EQUAL(ESP_OK, pick(one("\"2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL), &STABLE, &r));

#define REFUSED(json) TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pick(json, &ANY, &r))
    REFUSED(one("\"v2.3.4\"", ",\"draft\":true", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"nightly\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("234", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"P3A-V2.BIN\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin.sha256\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"starter\"", "3000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "1000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "99000000", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "\"3000000\"", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "-5", GOOD_DIGEST, GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", "null", GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", "\"sha256:0011\"", GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000",
                "\"sha512:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"", GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000",
                "\"sha256:0011223344556677889Xaabbccddeeff00112233445566778899aabbccddeeff\"", GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000",
                "\"sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00\"", GOOD_URL));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, "null"));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST,
                "\"http://github.com/fabkury/p3a/releases/download/v2.3.4/p3a-v2.bin\""));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST,
                "\"https://github.com/someone-else/p3a/releases/download/v2.3.4/p3a-v2.bin\""));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST,
                "\"" PREFIX "v2.3.4/p3a-v2.bin\\nHost: elsewhere\""));
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST,
                "\"" PREFIX "v2.3.4/p3a v2.bin\""));
    // An address too long to keep.
    char long_url[400];
    memset(long_url, 'a', sizeof(long_url));
    memcpy(long_url, "\"" PREFIX, strlen("\"" PREFIX));
    long_url[sizeof(long_url) - 2] = '"';
    long_url[sizeof(long_url) - 1] = '\0';
    REFUSED(one("\"v2.3.4\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, long_url));
#undef REFUSED
    // A version with a suffix is a pre-release whatever GitHub's flag says, and the flag makes one of any version.
    const char *suffixed = one("\"v2.3.4-rc.1\"", "", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pick(suffixed, &STABLE, &r));
    TEST_ASSERT_EQUAL(ESP_OK, pick(suffixed, &ANY, &r));
    TEST_ASSERT_TRUE(r.prerelease);
    const char *flagged = one("\"v2.3.4\"", ",\"prerelease\":true", "\"p3a-v2.bin\"", "\"uploaded\"", "3000000", GOOD_DIGEST, GOOD_URL);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pick(flagged, &STABLE, &r));
    TEST_ASSERT_EQUAL(ESP_OK, pick(flagged, &ANY, &r));
    TEST_ASSERT_TRUE(r.prerelease);
}

static void test_what_is_not_a_list_of_releases_is_damage_and_an_empty_list_is_nothing(void)
{
    update_release_t r;
    int seen = -1;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, update_release_pick("[]", 2, &ANY, &r, &seen));
    TEST_ASSERT_EQUAL(0, seen);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pick("[1, \"two\", null, {}, {\"tag_name\":\"v2.0.0\"}, {\"tag_name\":\"v2.0.0\",\"assets\":7}]", &ANY, &r));
    // GitHub's refusals are objects, not lists.
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, pick("{\"message\":\"API rate limit exceeded\"}", &ANY, &r));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, pick("", &ANY, &r));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, pick("<html>", &ANY, &r));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, pick("[{\"tag_name\":", &ANY, &r));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, update_release_pick(NULL, 0, &ANY, &r, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, update_release_pick("[]", 2, NULL, &r, NULL));
    const update_want_t no_prefix = {.prereleases = true, .max_bytes = SLOT, .url_prefix = ""};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, update_release_pick("[]", 2, &no_prefix, &r, NULL));
}

static void test_long_notes_are_cut_at_a_character(void)
{
    // 600 two-byte characters: 1200 bytes, more than the notes hold.
    static char json[4096];
    char body[1300];
    for (int i = 0; i < 600; i++) {
        body[2 * i] = '\xc3';
        body[2 * i + 1] = '\xa9';
    }
    body[1200] = '\0';
    snprintf(json, sizeof(json),
             "[{\"tag_name\":\"v2.3.4\",\"body\":\"%s\",\"assets\":[{\"name\":\"p3a-v2.bin\",\"size\":3000000,"
             "\"digest\":" GOOD_DIGEST ",\"browser_download_url\":" GOOD_URL "}]}]",
             body);
    update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, pick(json, &STABLE, &r));
    const size_t n = strlen(r.notes);
    TEST_ASSERT_EQUAL(UPDATE_NOTES_LEN - 2, n);  // 511 whole characters: half of the 512th would fit, so none of it is kept
    TEST_ASSERT_EQUAL_HEX8(0xa9, (uint8_t)r.notes[n - 1]);
    TEST_ASSERT_EQUAL_STRING("", r.published);
}

static void test_cut_and_mutated_lists_do_no_harm(void)
{
    size_t len = 0;
    char *json = read_file("github_releases_2x_made.json", &len);
    char *copy = malloc(len + 1);
    update_release_t r;
    uint32_t seed = 4242;
    for (size_t cut = 0; cut < len; cut += 211) {
        memcpy(copy, json, cut);
        copy[cut] = '\0';
        update_release_pick(copy, cut, &ANY, &r, NULL);
    }
    int offered = 0;
    for (int round = 0; round < 1500; round++) {
        memcpy(copy, json, len + 1);
        for (int k = 0; k < 1 + round % 6; k++) {
            seed = seed * 1664525u + 1013904223u;
            const size_t at = (seed >> 8) % len;
            seed = seed * 1664525u + 1013904223u;
            copy[at] = (char)(1 + (seed >> 16) % 255);
        }
        if (update_release_pick(copy, len, &ANY, &r, NULL) == ESP_OK) {
            offered++;
            // Whatever the damage, what is offered is a version, at an address under the prefix, of a size that fits.
            TEST_ASSERT_TRUE(update_version_ok(r.version));
            TEST_ASSERT_EQUAL(0, strncmp(r.url, PREFIX, strlen(PREFIX)));
            TEST_ASSERT_TRUE(r.size >= UPDATE_MIN_BYTES && r.size <= SLOT);
            TEST_ASSERT_TRUE(strlen(r.notes) < UPDATE_NOTES_LEN && strlen(r.published) < UPDATE_DATE_LEN);
        }
    }
    TEST_ASSERT_TRUE(offered > 100);  // most damage lands in fields that do not matter
    free(copy);
    free(json);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_versions_are_ordered_as_semantic_versioning_orders_them);
    RUN_TEST(test_what_is_not_a_version_is_refused_and_sorts_first);
    RUN_TEST(test_the_list_recorded_while_only_1x_had_shipped_has_nothing_for_a_2x_device);
    RUN_TEST(test_the_highest_version_with_a_whole_image_is_offered);
    RUN_TEST(test_a_release_is_passed_over_for_each_thing_that_makes_it_unusable);
    RUN_TEST(test_what_is_not_a_list_of_releases_is_damage_and_an_empty_list_is_nothing);
    RUN_TEST(test_long_notes_are_cut_at_a_character);
    RUN_TEST(test_cut_and_mutated_lists_do_no_harm);
    return UNITY_END();
}
