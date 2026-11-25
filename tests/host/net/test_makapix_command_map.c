// SPDX-License-Identifier: Apache-2.0
// What each command from Makapix Club asks the device to do: the payloads
// as the server's own source builds them, 1.x's forms, and payloads that
// will not do.
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "makapix_command_map.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static makapix_action_t a;

static makapix_do_t map(const char *type, const char *payload)
{
    makapix_action_free(&a);
    cJSON *json = payload != NULL ? cJSON_Parse(payload) : NULL;
    if (payload != NULL) {
        TEST_ASSERT_NOT_NULL_MESSAGE(json, payload);
    }
    makapix_command_map(type, json, &a);
    cJSON_Delete(json);
    if (a.kind != MAKAPIX_DO_PLAYSET) {
        TEST_ASSERT_NULL(a.playset);
    }
    return a.kind;
}

static void test_next_back_and_commands_nobody_knows(void)
{
    TEST_ASSERT_EQUAL(MAKAPIX_DO_NEXT, map("swap_next", "{}"));
    TEST_ASSERT_FALSE(a.wants_ack);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_BACK, map("swap_back", NULL));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_NOTHING, map("add_device", "{}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_NOTHING, map("", "{}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_NOTHING, map(NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("next", makapix_do_name(MAKAPIX_DO_NEXT));
    TEST_ASSERT_EQUAL_STRING("rotation", makapix_do_name(MAKAPIX_DO_ROTATION));
    TEST_ASSERT_EQUAL_STRING("?", makapix_do_name((makapix_do_t)99));
}

static void test_an_artwork_to_show_becomes_a_record(void)
{
    // As the server sends it.
    TEST_ASSERT_EQUAL(MAKAPIX_DO_SHOW,
                      map("show_artwork", "{\"post_id\":4028,\"title\":\"yuh\",\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\","
                                          "\"storage_shard\":\"06/04\",\"native_format\":\"webp\",\"width\":160,\"height\":160}"));
    TEST_ASSERT_EQUAL(P3A_SOURCE_MAKAPIX, a.record.source);
    TEST_ASSERT_EQUAL_UINT32(4028, a.record.id32);
    TEST_ASSERT_EQUAL_HEX8(0xF1, a.record.identity.makapix.uuid[0]);
    TEST_ASSERT_EQUAL(P3A_FORMAT_WEBP, a.record.format);
    TEST_ASSERT_EQUAL(160, a.record.width);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&a.record));
    // swap_to names the file by its address.
    TEST_ASSERT_EQUAL(MAKAPIX_DO_SHOW,
                      map("swap_to", "{\"channel\":\"all\",\"post_id\":7,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\","
                                     "\"art_url\":\"https://vault.makapix.club/06/04/f14ebf48-c4a3-449f-92c1-58a83224d984.gif\"}"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_GIF, a.record.format);
    TEST_ASSERT_EQUAL(0, a.record.width);

    static const char *const bad[] = {
        "{}",
        "{\"post_id\":0,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\",\"native_format\":\"webp\"}",
        "{\"post_id\":-4,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\",\"native_format\":\"webp\"}",
        "{\"post_id\":4.5,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\",\"native_format\":\"webp\"}",
        "{\"post_id\":4028,\"storage_key\":\"../../etc\",\"native_format\":\"webp\"}",
        "{\"post_id\":4028,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\",\"native_format\":\"tiff\"}",
        "{\"post_id\":4028,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\"}",
        "{\"post_id\":\"4028\",\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\",\"native_format\":\"webp\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(MAKAPIX_DO_REFUSE, map("show_artwork", bad[i]), bad[i]);
        TEST_ASSERT_TRUE(strlen(a.text) > 10);
    }
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("show_artwork", NULL));
    // swap_to does not take the format by name: its message carries the address.
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE,
                      map("swap_to", "{\"post_id\":7,\"storage_key\":\"f14ebf48-c4a3-449f-92c1-58a83224d984\",\"native_format\":\"gif\"}"));
}

static void test_a_file_or_an_address_to_show(void)
{
    TEST_ASSERT_EQUAL(MAKAPIX_DO_SHOW_FILE, map("swap_to", "{\"channel\":\"sdcard\",\"filename\":\"my cat.GIF\"}"));
    TEST_ASSERT_EQUAL_STRING("my cat.GIF", a.text);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("swap_to", "{\"channel\":\"sdcard\"}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("swap_to", "{\"channel\":\"sdcard\",\"filename\":\"../settings.gif\"}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("swap_to", "{\"channel\":\"sdcard\",\"filename\":\"a\\\\b.gif\"}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("swap_to", "{\"channel\":\"sdcard\",\"filename\":\"notes.txt\"}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("swap_to", "{\"channel\":\"sdcard\",\"filename\":\".hidden.gif\"}"));

    TEST_ASSERT_EQUAL(MAKAPIX_DO_SHOW_URL, map("show_url", "{\"artwork_url\":\"https://example.org/a.gif\",\"blocking\":true}"));
    TEST_ASSERT_EQUAL_STRING("https://example.org/a.gif", a.text);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("show_url", "{\"artwork_url\":\"ftp://example.org/a.gif\"}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("show_url", "{\"artwork_url\":7}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("show_url", "{}"));
}

static void test_a_channel_to_play_is_a_playset_of_one(void)
{
    static const struct {
        const char *payload;
        uint8_t source;
        const char *kind, *identifier;
    } good[] = {
        {"{\"channel_name\":\"all\"}", PLAYSET_SOURCE_MAKAPIX, "all", ""},
        {"{\"channel_name\":\"promoted\"}", PLAYSET_SOURCE_MAKAPIX, "promoted", ""},
        {"{\"channel_name\":\"sdcard\"}", PLAYSET_SOURCE_LOCAL, "", ""},
        {"{\"channel_name\":\"by_user\",\"user_sqid\":\"QVJ\",\"user_handle\":\"birds\"}", PLAYSET_SOURCE_MAKAPIX, "by_user", "QVJ"},
        {"{\"channel_name\":\"reactions\",\"user_sqid\":\"QVJ\"}", PLAYSET_SOURCE_MAKAPIX, "reactions", "QVJ"},
        {"{\"channel_name\":\"hashtag\",\"hashtag\":\"pixelart\"}", PLAYSET_SOURCE_MAKAPIX, "hashtag", "pixelart"},
        {"{\"channel_name\":\"pinned\"}", PLAYSET_SOURCE_PINNED, "", ""},
        {"{\"channel_name\":\"pinned\",\"slug\":\"0badf00d\"}", PLAYSET_SOURCE_PINNED, "", "0badf00d"},
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(MAKAPIX_DO_PLAYSET, map("play_channel", good[i].payload), good[i].payload);
        TEST_ASSERT_EQUAL(1, a.playset->count);
        TEST_ASSERT_EQUAL(good[i].source, a.playset->channels[0].source);
        TEST_ASSERT_EQUAL_STRING(good[i].kind, a.playset->channels[0].kind);
        TEST_ASSERT_EQUAL_STRING(good[i].identifier, a.playset->channels[0].identifier);
        TEST_ASSERT_EQUAL_STRING("", a.playset->name);
        TEST_ASSERT_FALSE(a.save);
        TEST_ASSERT_EQUAL(ESP_OK, playset_check(a.playset, NULL, 0));
    }
    static const char *const bad[] = {
        "{}",
        "{\"channel_name\":\"by_user\"}",
        "{\"channel_name\":\"by_user\",\"user_sqid\":\"\"}",
        "{\"channel_name\":\"hashtag\"}",
        "{\"channel_name\":\"artwork\"}",
        "{\"channel_name\":\"giphy\"}",
        "{\"channel_name\":7}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(MAKAPIX_DO_REFUSE, map("play_channel", bad[i]), bad[i]);
    }
    // An identifier longer than a channel holds is refused, not cut.
    char payload[400] = "{\"channel_name\":\"hashtag\",\"hashtag\":\"";
    memset(payload + strlen(payload), 'x', 200);
    strcat(payload, "\"}");
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("play_channel", payload));
}

static void test_a_playset_in_the_servers_form_in_1x_s_and_in_p3as_own(void)
{
    // followed_artists, as the server generates it.
    static const char followed[] =
        "{\"playset_name\":\"followed_artists\",\"channels\":["
        "{\"type\":\"user\",\"identifier\":\"QVJ\",\"display_name\":\"@birds\"},"
        "{\"type\":\"user\",\"identifier\":\"k3\",\"display_name\":\"@fab\",\"weight\":3},"
        "{\"type\":\"named\",\"name\":\"promoted\"},{\"type\":\"hashtag\",\"identifier\":\"cats\"},{\"type\":\"sdcard\"}],"
        "\"exposure_mode\":\"equal\",\"pick_mode\":\"random\"}";
    TEST_ASSERT_EQUAL(MAKAPIX_DO_PLAYSET, map("play_playset", followed));
    TEST_ASSERT_EQUAL(5, a.playset->count);
    TEST_ASSERT_EQUAL_STRING("followed_artists", a.playset->name);
    TEST_ASSERT_FALSE_MESSAGE(a.save, "the server's name says what it is; it is not an order to keep it");
    TEST_ASSERT_EQUAL_STRING("by_user", a.playset->channels[0].kind);
    TEST_ASSERT_EQUAL_STRING("QVJ", a.playset->channels[0].identifier);
    TEST_ASSERT_EQUAL_STRING("@birds", a.playset->channels[0].display_name);
    TEST_ASSERT_EQUAL(3, a.playset->channels[1].weight);
    TEST_ASSERT_EQUAL_STRING("promoted", a.playset->channels[2].kind);
    TEST_ASSERT_EQUAL_STRING("hashtag", a.playset->channels[3].kind);
    TEST_ASSERT_EQUAL(PLAYSET_SOURCE_LOCAL, a.playset->channels[4].source);
    TEST_ASSERT_EQUAL(ESP_OK, playset_check(a.playset, NULL, 0));
    // 1.x's name for the command, with "name" to save it too.
    TEST_ASSERT_EQUAL(MAKAPIX_DO_PLAYSET,
                      map("execute_playset", "{\"name\":\"Evening\",\"channels\":[{\"type\":\"named\",\"name\":\"all\"}]}"));
    TEST_ASSERT_TRUE(a.save);
    TEST_ASSERT_EQUAL_STRING("Evening", a.playset->name);
    // p3a's own form, read by the playset's own reader.
    TEST_ASSERT_EQUAL(MAKAPIX_DO_PLAYSET,
                      map("play_playset", "{\"name\":\"Mix\",\"channels\":[{\"source\":\"giphy\",\"kind\":\"search\",\"identifier\":\"cats\","
                                          "\"weight\":2},{\"source\":\"local\"}]}"));
    TEST_ASSERT_EQUAL(2, a.playset->count);
    TEST_ASSERT_EQUAL(PLAYSET_SOURCE_GIPHY, a.playset->channels[0].source);
    TEST_ASSERT_TRUE(a.save);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_PLAYSET, map("play_playset", "{\"channels\":[{\"source\":\"local\"}]}"));
    TEST_ASSERT_FALSE(a.save);

    static const char *const bad[] = {
        "{}",
        "{\"channels\":[]}",
        "{\"channels\":7}",
        "{\"channels\":[{\"type\":\"named\",\"name\":\"trending\"}]}",
        "{\"channels\":[{\"type\":\"user\"}]}",
        "{\"channels\":[{\"type\":\"reactions\",\"identifier\":\"QVJ\"}]}",
        "{\"channels\":[{}]}",
        "{\"channels\":[7]}",
        "{\"channels\":[{\"source\":\"nonesuch\"}]}",
        "{\"channels\":[{\"source\":\"local\",\"surprise\":1}]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(MAKAPIX_DO_REFUSE, map("play_playset", bad[i]), bad[i]);
    }
    // More channels than a playset holds.
    cJSON *many = cJSON_CreateObject();
    cJSON *channels = cJSON_AddArrayToObject(many, "channels");
    for (int i = 0; i < PLAYSET_MAX_CHANNELS + 1; i++) {
        cJSON *channel = cJSON_CreateObject();
        cJSON_AddStringToObject(channel, "type", "sdcard");
        cJSON_AddItemToArray(channels, channel);
    }
    makapix_action_free(&a);
    makapix_command_map("play_playset", many, &a);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, a.kind);
    TEST_ASSERT_NULL(a.playset);
    cJSON_Delete(many);
}

static void test_the_settings_commands_and_their_acknowledgement(void)
{
    TEST_ASSERT_EQUAL(MAKAPIX_DO_PAUSE, map("set_paused", "{\"paused\":true}"));
    TEST_ASSERT_TRUE(a.wants_ack);
    TEST_ASSERT_EQUAL(1, a.value);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_PAUSE, map("set_paused", "{\"paused\":false}"));
    TEST_ASSERT_EQUAL(0, a.value);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("set_paused", "{\"paused\":1}"));
    TEST_ASSERT_TRUE_MESSAGE(a.wants_ack, "a refusal is acknowledged too, as an error");
    TEST_ASSERT_EQUAL_STRING("missing or invalid 'paused' field", a.text);

    TEST_ASSERT_EQUAL(MAKAPIX_DO_BRIGHTNESS, map("set_brightness", "{\"value\":40}"));
    TEST_ASSERT_EQUAL(40, a.value);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_BRIGHTNESS, map("set_brightness", "{\"value\":250}"));
    TEST_ASSERT_EQUAL(100, a.value);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_BRIGHTNESS, map("set_brightness", "{\"value\":-3}"));
    TEST_ASSERT_EQUAL(0, a.value);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("set_brightness", "{\"value\":\"bright\"}"));
    TEST_ASSERT_TRUE(a.wants_ack);

    TEST_ASSERT_EQUAL(MAKAPIX_DO_ROTATION, map("set_rotation", "{\"value\":270}"));
    TEST_ASSERT_EQUAL(270, a.value);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_ROTATION, map("set_rotation", "{\"value\":0}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("set_rotation", "{\"value\":45}"));
    TEST_ASSERT_EQUAL_STRING("rotation must be 0, 90, 180, or 270", a.text);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("set_rotation", "{}"));

    TEST_ASSERT_EQUAL(MAKAPIX_DO_UNSUPPORTED, map("set_mirror", "{\"value\":\"horizontal\"}"));
    TEST_ASSERT_TRUE(a.wants_ack);

    TEST_ASSERT_EQUAL(MAKAPIX_DO_BACKGROUND, map("set_background_color", "{\"r\":255,\"g\":0,\"b\":16}"));
    TEST_ASSERT_EQUAL_STRING("#FF0010", a.text);
    TEST_ASSERT_FALSE(a.wants_ack);
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("set_background_color", "{\"r\":256,\"g\":0,\"b\":0}"));
    TEST_ASSERT_EQUAL(MAKAPIX_DO_REFUSE, map("set_background_color", "{\"r\":1,\"g\":2}"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_next_back_and_commands_nobody_knows);
    RUN_TEST(test_an_artwork_to_show_becomes_a_record);
    RUN_TEST(test_a_file_or_an_address_to_show);
    RUN_TEST(test_a_channel_to_play_is_a_playset_of_one);
    RUN_TEST(test_a_playset_in_the_servers_form_in_1x_s_and_in_p3as_own);
    RUN_TEST(test_the_settings_commands_and_their_acknowledgement);
    makapix_action_free(&a);
    return UNITY_END();
}
