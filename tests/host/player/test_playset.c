// SPDX-License-Identifier: Apache-2.0
// The playset: its JSON form (what is accepted, what is refused and why),
// the channel id, and the packed form read back from damaged bytes.
#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "playset.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static playset_t a, b;  // 14 KB each: not on the stack
static char why[160];

static esp_err_t parse(const char *text, playset_t *out)
{
    cJSON *json = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, text);
    why[0] = '\0';
    esp_err_t err = playset_from_json(json, out, why, sizeof(why));
    cJSON_Delete(json);
    return err;
}

static void test_the_layout_and_the_numbers_on_disk(void)
{
    TEST_ASSERT_EQUAL(224, sizeof(playset_channel_t));
    TEST_ASSERT_EQUAL(4, offsetof(playset_channel_t, weight));
    TEST_ASSERT_EQUAL(12, offsetof(playset_channel_t, kind));
    TEST_ASSERT_EQUAL(44, offsetof(playset_channel_t, identifier));
    TEST_ASSERT_EQUAL(140, offsetof(playset_channel_t, display_name));
    TEST_ASSERT_EQUAL(204, offsetof(playset_channel_t, refresh_s));
    TEST_ASSERT_EQUAL(208, offsetof(playset_channel_t, cache_size));
    TEST_ASSERT_EQUAL(210, offsetof(playset_channel_t, reserved));
    TEST_ASSERT_EQUAL(1, PLAYSET_SOURCE_LOCAL);
    TEST_ASSERT_EQUAL(5, PLAYSET_SOURCE_MUSEUM);
    TEST_ASSERT_EQUAL(6, PLAYSET_SOURCE_PINNED);
    TEST_ASSERT_EQUAL_STRING("klipy", playset_source_name(PLAYSET_SOURCE_KLIPY));
    TEST_ASSERT_NULL(playset_source_name(0));
    TEST_ASSERT_NULL(playset_source_name(200));
    TEST_ASSERT_EQUAL(PLAYSET_SOURCE_GIPHY, playset_source_parse("giphy"));
    TEST_ASSERT_EQUAL(0, playset_source_parse("Giphy"));
    TEST_ASSERT_EQUAL(0, playset_source_parse(NULL));
}

static void test_json_goes_in_and_comes_back_the_same(void)
{
    const char *text =
        "{\"name\":\"Evening mix\",\"channels\":["
        "{\"source\":\"makapix\",\"kind\":\"promoted\",\"weight\":3},"
        "{\"source\":\"giphy\",\"kind\":\"search\",\"identifier\":\"pixel cats\",\"display_name\":\"Cats\",\"weight\":1},"
        "{\"source\":\"museum\",\"kind\":\"cma:department\",\"identifier\":\"Japanese Art\",\"offset\":200,"
        "\"cache_size\":64,\"refresh_s\":172800},"
        "{\"source\":\"local\"}]}";
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, parse(text, &a), why);
    TEST_ASSERT_EQUAL_STRING("Evening mix", a.name);
    TEST_ASSERT_EQUAL(4, a.count);
    TEST_ASSERT_EQUAL(PLAYSET_SOURCE_GIPHY, a.channels[1].source);
    TEST_ASSERT_EQUAL_STRING("pixel cats", a.channels[1].identifier);
    TEST_ASSERT_EQUAL(200, a.channels[2].offset);
    TEST_ASSERT_EQUAL(0, a.channels[3].weight);
    TEST_ASSERT_EQUAL(64, a.channels[2].cache_size);
    TEST_ASSERT_EQUAL(172800, a.channels[2].refresh_s);
    TEST_ASSERT_EQUAL(0, a.channels[1].cache_size);  // not given: the source's setting
    TEST_ASSERT_EQUAL(0, a.channels[1].refresh_s);

    cJSON *json = playset_to_json(&a);
    TEST_ASSERT_NOT_NULL(json);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "channels"), 0);
    TEST_ASSERT_EQUAL(16, strlen(cJSON_GetObjectItem(first, "id")->valuestring));
    const cJSON *third = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "channels"), 2);
    TEST_ASSERT_EQUAL(64, cJSON_GetObjectItem(third, "cache_size")->valueint);
    TEST_ASSERT_EQUAL(172800, cJSON_GetObjectItem(third, "refresh_s")->valueint);
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(first, "cache_size")->valueint);
    // What was given out is accepted back, its "id" keys included.
    TEST_ASSERT_EQUAL(ESP_OK, playset_from_json(json, &b, why, sizeof(why)));
    cJSON_Delete(json);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

static void test_what_does_not_fit_is_refused_with_a_reason(void)
{
    char text[400], long_text[130];
    memset(long_text, 'x', sizeof(long_text));

    long_text[95] = '\0';  // 95 characters: the longest identifier
    snprintf(text, sizeof(text), "{\"channels\":[{\"source\":\"museum\",\"identifier\":\"%s\"}]}", long_text);
    TEST_ASSERT_EQUAL(ESP_OK, parse(text, &a));
    TEST_ASSERT_EQUAL(95, strlen(a.channels[0].identifier));
    long_text[95] = 'x';
    long_text[96] = '\0';  // one more
    snprintf(text, sizeof(text), "{\"channels\":[{\"source\":\"museum\",\"identifier\":\"%s\"}]}", long_text);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(text, &a));
    TEST_ASSERT_NOT_NULL(strstr(why, "96 characters; at most 95"));
    TEST_ASSERT_EQUAL(0, a.count);

    long_text[33] = '\0';  // a name of 33
    snprintf(text, sizeof(text), "{\"name\":\"%s\",\"channels\":[{\"source\":\"local\"}]}", long_text);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(text, &a));
    TEST_ASSERT_NOT_NULL(strstr(why, "\"name\""));
}

static void test_wrong_shapes_are_refused(void)
{
    const char *bad[] = {
        "[]",
        "{}",
        "{\"channels\":[]}",
        "{\"channels\":{}}",
        "{\"channels\":[7]}",
        "{\"channels\":[{}]}",
        "{\"channels\":[{\"source\":\"youtube\"}]}",
        "{\"channels\":[{\"source\":3}]}",
        "{\"channels\":[{\"source\":\"local\",\"weight\":-1}]}",
        "{\"channels\":[{\"source\":\"local\",\"weight\":1.5}]}",
        "{\"channels\":[{\"source\":\"local\",\"weight\":1000001}]}",
        "{\"channels\":[{\"source\":\"local\",\"weight\":\"2\"}]}",
        "{\"channels\":[{\"source\":\"local\",\"offset\":4294967296}]}",
        "{\"channels\":[{\"source\":\"local\",\"kind\":5}]}",
        "{\"channels\":[{\"source\":\"giphy\",\"cache_size\":4097}]}",
        "{\"channels\":[{\"source\":\"giphy\",\"cache_size\":-1}]}",
        "{\"channels\":[{\"source\":\"giphy\",\"cache_size\":\"100\"}]}",
        "{\"channels\":[{\"source\":\"giphy\",\"refresh_s\":4294967296}]}",
        "{\"channels\":[{\"source\":\"giphy\",\"refresh_s\":60.5}]}",
        "{\"channels\":[{\"source\":\"local\",\"wieght\":2}]}",   // a typo is not silently dropped
        "{\"channels\":[{\"source\":\"local\"}],\"pick_mode\":\"random\"}",
        "{\"name\":\"a/b\",\"channels\":[{\"source\":\"local\"}]}",
        "{\"name\":\"a\\nb\",\"channels\":[{\"source\":\"local\"}]}",
        "{\"name\":7,\"channels\":[{\"source\":\"local\"}]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, parse(bad[i], &a), bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(why[0] != '\0', bad[i]);
    }
    // 64 channels are taken, 65 are not.
    static char many[65 * 22 + 40];
    for (int count = 64; count <= 65; count++) {
        strcpy(many, "{\"channels\":[");
        for (int i = 0; i < count; i++) {
            strcat(many, i ? ",{\"source\":\"local\"}" : "{\"source\":\"local\"}");
        }
        strcat(many, "]}");
        TEST_ASSERT_EQUAL(count == 64 ? ESP_OK : ESP_ERR_INVALID_ARG, parse(many, &a));
    }
}

static void test_the_channel_id_names_the_channel_not_its_trimmings(void)
{
    char id[PLAYSET_CHANNEL_ID_LEN], other[PLAYSET_CHANNEL_ID_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"channels\":["
                                    "{\"source\":\"giphy\",\"kind\":\"search\",\"identifier\":\"cats\"},"
                                    "{\"source\":\"giphy\",\"kind\":\"search\",\"identifier\":\"cats\",\"weight\":9,"
                                    "\"display_name\":\"Kitties\",\"cache_size\":100,\"refresh_s\":3600},"
                                    "{\"source\":\"klipy\",\"kind\":\"search\",\"identifier\":\"cats\"},"
                                    "{\"source\":\"giphy\",\"kind\":\"search\",\"identifier\":\"cats\",\"offset\":1},"
                                    "{\"source\":\"giphy\",\"kind\":\"searchc\",\"identifier\":\"ats\"},"
                                    "{\"source\":\"giphy\",\"kind\":\"search\",\"identifier\":\"Cats\"}]}",
                                    &a));
    playset_channel_id(&a.channels[0], id);
    TEST_ASSERT_EQUAL(16, strlen(id));
    playset_channel_id(&a.channels[1], other);
    TEST_ASSERT_EQUAL_STRING(id, other);  // weight, display name, size and interval do not make another channel
    for (int i = 2; i < 6; i++) {         // source, offset, where the text is split, and case do
        playset_channel_id(&a.channels[i], other);
        TEST_ASSERT_TRUE(strcmp(id, other) != 0);
    }
    // Written into file names: the value itself must never change.
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"channels\":[{\"source\":\"local\"}]}", &a));
    playset_channel_id(&a.channels[0], id);
    TEST_ASSERT_EQUAL_STRING("b2389e8a64b397cc", id);
}

static void test_packing_round_trips_and_damaged_records_are_refused(void)
{
    static uint8_t records[(PLAYSET_MAX_CHANNELS + 1) * PLAYSET_RECORD_SIZE];
    static uint8_t damaged[sizeof(records)];
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"name\":\"Mix\",\"channels\":["
                                    "{\"source\":\"makapix\",\"kind\":\"all\",\"weight\":2,\"cache_size\":100,"
                                    "\"refresh_s\":7200},"
                                    "{\"source\":\"pinned\",\"identifier\":\"favourites\",\"display_name\":\"Favs\"},"
                                    "{\"source\":\"local\"}]}",
                                    &a));
    const uint32_t count = playset_pack(&a, records);
    TEST_ASSERT_EQUAL(4, count);
    TEST_ASSERT_EQUAL(ESP_OK, playset_unpack(records, count, &b));
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));

    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, playset_unpack(records, 1, &b));   // a name and no channels
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, playset_unpack(records, 0, &b));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, playset_unpack(records, 66, &b));
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, playset_unpack(NULL, 4, &b));

    // Every single-byte change either still reads as a sound playset or is
    // refused; under the sanitizers, nothing reads outside the records.
    const size_t len = (size_t)count * PLAYSET_RECORD_SIZE;
    unsigned refused = 0;
    for (size_t at = 0; at < len; at++) {
        for (int value = 0; value < 3; value++) {
            memcpy(damaged, records, len);
            damaged[at] = (uint8_t)(value == 0 ? 0xFF : value == 1 ? damaged[at] ^ 0x01 : 'A');
            const esp_err_t err = playset_unpack(damaged, count, &b);
            if (err != ESP_OK) {
                TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, err);
                TEST_ASSERT_EQUAL(0, b.count);
                refused++;
            } else {
                TEST_ASSERT_EQUAL(ESP_OK, playset_check(&b, NULL, 0));
            }
        }
    }
    TEST_ASSERT_TRUE(refused > len);  // most damage is caught here; the container's checksum catches the rest
}

static void test_a_channels_own_value_is_held_within_what_its_source_allows(void)
{
    // Not set: the source's.
    TEST_ASSERT_EQUAL(350, playset_value(0, 350, 32, 500));
    // Set, and within the range.
    TEST_ASSERT_EQUAL(100, playset_value(100, 350, 32, 500));
    TEST_ASSERT_EQUAL(32, playset_value(32, 350, 32, 500));
    TEST_ASSERT_EQUAL(500, playset_value(500, 350, 32, 500));
    // Set outside it (a file written when the range was another): held within.
    TEST_ASSERT_EQUAL(32, playset_value(5, 350, 32, 500));
    TEST_ASSERT_EQUAL(500, playset_value(4096, 350, 32, 500));
    // A source that allows none (the card, a pin list) gives its own whatever is asked.
    TEST_ASSERT_EQUAL(4096, playset_value(100, 4096, 0, 0));
    TEST_ASSERT_EQUAL(0, playset_value(3600, 0, 0, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_channels_own_value_is_held_within_what_its_source_allows);
    RUN_TEST(test_the_layout_and_the_numbers_on_disk);
    RUN_TEST(test_json_goes_in_and_comes_back_the_same);
    RUN_TEST(test_what_does_not_fit_is_refused_with_a_reason);
    RUN_TEST(test_wrong_shapes_are_refused);
    RUN_TEST(test_the_channel_id_names_the_channel_not_its_trimmings);
    RUN_TEST(test_packing_round_trips_and_damaged_records_are_refused);
    return UNITY_END();
}
