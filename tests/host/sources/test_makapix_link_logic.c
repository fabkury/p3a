// SPDX-License-Identifier: Apache-2.0
// The Makapix link's decisions and text: when to try again and when to
// give up, when a view is due, and what the player says.
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "makapix_link_logic.h"
#include "makapix_parse.h"
#include "p3a_err.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_waits_grow_to_two_minutes_and_are_spread(void)
{
    makapix_retry_t r;
    makapix_retry_reset(&r);
    static const uint32_t bases[] = {15000, 30000, 60000, 120000, 120000, 120000};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        makapix_retry_t low = r, high = r, mid = r;
        const uint32_t hi = bases[i] + bases[i] / 2 > 120000 ? 120000 : bases[i] + bases[i] / 2;
        const uint32_t lo = bases[i] / 2;
        TEST_ASSERT_EQUAL_UINT32(lo, makapix_retry_delay(&low, 0));
        TEST_ASSERT_EQUAL_UINT32(hi, makapix_retry_delay(&high, hi - lo));
        const uint32_t any = makapix_retry_delay(&mid, 0xDEADBEEFu + (uint32_t)i * 7919u);
        TEST_ASSERT_TRUE(any >= lo && any <= hi);
        makapix_retry_delay(&r, 0);
    }
    // A connection made starts over.
    makapix_retry_reset(&r);
    TEST_ASSERT_EQUAL_UINT32(7500, makapix_retry_delay(&r, 0));
    // A structure that was never set up does not divide by zero or wait for ever.
    makapix_retry_t wild = {.base_ms = 0};
    TEST_ASSERT_TRUE(makapix_retry_delay(&wild, 12345) <= 22500);
    wild.base_ms = 0xFFFFFFFFu;
    TEST_ASSERT_TRUE(makapix_retry_delay(&wild, 12345) <= 120000);
}

static void test_three_failed_handshakes_call_for_one_renewal_then_the_registration_is_invalid(void)
{
    makapix_retry_t r;
    makapix_retry_reset(&r);
    // The network being down says nothing about the certificates, however often.
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_failed(&r, false, false));
    }
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_failed(&r, true, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_failed(&r, true, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_RENEW, makapix_retry_failed(&r, true, false));
    // The renewal could not be tried (no clock): asked for again at the next failure.
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_renewed(&r, false, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_RENEW, makapix_retry_failed(&r, true, false));
    // It worked: three more failures with the new certificates and it is not the certificates.
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_renewed(&r, true, true));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_failed(&r, true, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_failed(&r, true, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_INVALID, makapix_retry_failed(&r, true, false));

    // A renewal that the server refuses ends it at once.
    makapix_retry_reset(&r);
    makapix_retry_failed(&r, true, false);
    makapix_retry_failed(&r, true, false);
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_RENEW, makapix_retry_failed(&r, true, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_INVALID, makapix_retry_renewed(&r, false, true));

    // The broker's own certificate rejected (a new CA): renewal at the first failure.
    makapix_retry_reset(&r);
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_RENEW, makapix_retry_failed(&r, true, true));
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_renewed(&r, true, true));
    // A connection made forgives everything.
    makapix_retry_reset(&r);
    TEST_ASSERT_EQUAL(MAKAPIX_RETRY_AGAIN, makapix_retry_failed(&r, true, false));
}

static void test_a_renewal_is_due_late_in_a_certificates_life_and_never_without_a_clock(void)
{
    const uint32_t day = 86400u;
    const uint32_t ends = 1900000000u;  // some day in 2030
    // Long before: no.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NOT_DUE, makapix_renew_due(ends - 400 * day, ends, 0, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NOT_DUE, makapix_renew_due(ends - 190 * day - 1, ends, 0, false));
    // The window opens at 190 days, for the player whose draw is zero...
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends - 190 * day, ends, 0, false));
    // ...and up to a week later for the others.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NOT_DUE, makapix_renew_due(ends - 190 * day, ends, 3 * day, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NOT_DUE, makapix_renew_due(ends - 187 * day - 1, ends, 3 * day, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends - 187 * day, ends, 3 * day, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends - 183 * day, ends, 0xFFFFFFFFu, false));  // a week on: everyone
    // With two weeks left, at every check; and after the end.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends - 14 * day, ends, 0xFFFFFFFFu, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends, ends, 5, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends + 900 * day, ends, 5, false));
    // Asked for outright (the broker rejected the player): whatever the dates.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends - 400 * day, ends, 5, true));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(ends - 400 * day, 0, 5, true));
    // A certificate whose end could not be read is renewed only when asked for.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NOT_DUE, makapix_renew_due(ends, 0, 5, false));
    // No clock: nothing can be told, forced or not.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NO_CLOCK, makapix_renew_due(0, ends, 5, false));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NO_CLOCK, makapix_renew_due(1700000000u, ends, 5, true));
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_NO_CLOCK, makapix_renew_due(MAKAPIX_CLOCK_PLAUSIBLE - 1, ends, 5, true));
    // A certificate that ends sooner than 190 days after 1970 does not wrap the arithmetic.
    TEST_ASSERT_EQUAL(MAKAPIX_RENEW_DUE, makapix_renew_due(MAKAPIX_CLOCK_PLAUSIBLE, 100 * day, 5, false));
}

static void test_a_view_is_due_after_five_seconds_seen_and_every_thirty_after(void)
{
    makapix_views_t v;
    makapix_views_init(&v);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 100000));  // nothing shown
    makapix_views_show(&v, 4028, 1000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 1000));
    TEST_ASSERT_FALSE(makapix_views_due(&v, 5999));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 6000));
    TEST_ASSERT_FALSE(makapix_views_due(&v, 6001));  // once
    TEST_ASSERT_FALSE(makapix_views_due(&v, 35999));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 36000));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 66000));
    // The same artwork picked again carries on; another starts over.
    makapix_views_show(&v, 4028, 70000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 80000));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 96000));
    makapix_views_show(&v, 4033, 100000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 104999));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 105000));
    // Nothing of Makapix's on the panel: nothing is due however long.
    makapix_views_show(&v, 0, 106000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 999000));
}

static void test_time_covered_does_not_count_towards_a_view(void)
{
    makapix_views_t v;
    makapix_views_init(&v);
    makapix_views_show(&v, 7, 0);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 3000));
    makapix_views_cover(&v, true, 3000);   // a screen over it, seen for 3 s
    TEST_ASSERT_FALSE(makapix_views_due(&v, 60000));
    makapix_views_cover(&v, false, 60000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 61999));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 62000));  // 3 s before, 2 s after
    // Covered twice running, uncovered twice running: no time is counted twice or lost.
    makapix_views_cover(&v, true, 70000);
    makapix_views_cover(&v, true, 80000);
    makapix_views_cover(&v, false, 90000);
    makapix_views_cover(&v, false, 95000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 111999));  // 13 s seen since the report: 8 before the cover, 5 after...
    TEST_ASSERT_TRUE(makapix_views_due(&v, 112000));   // ...and at 112000 it is 30
    // An artwork that comes up while covered waits for the cover to lift.
    makapix_views_show(&v, 8, 120000);
    makapix_views_cover(&v, true, 120000);
    makapix_views_show(&v, 9, 121000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 200000));
    makapix_views_cover(&v, false, 200000);
    TEST_ASSERT_FALSE(makapix_views_due(&v, 204999));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 205000));
    // A device that was busy and asks late reports once, and counts on from then.
    makapix_views_show(&v, 10, 300000);
    TEST_ASSERT_TRUE(makapix_views_due(&v, 400000));
    TEST_ASSERT_FALSE(makapix_views_due(&v, 400001));
    TEST_ASSERT_FALSE(makapix_views_due(&v, 429999));
    TEST_ASSERT_TRUE(makapix_views_due(&v, 430000));
}

static const char KEY[] = "550e8400-e29b-41d4-a716-446655440000";

static void test_topics_and_times(void)
{
    char topic[MAKAPIX_TOPIC_LEN], when[24];
    makapix_topic(MAKAPIX_TOPIC_COMMAND, KEY, topic);
    TEST_ASSERT_EQUAL_STRING("makapix/player/550e8400-e29b-41d4-a716-446655440000/command", topic);
    makapix_topic(MAKAPIX_TOPIC_ACK, KEY, topic);
    TEST_ASSERT_EQUAL_STRING("makapix/player/550e8400-e29b-41d4-a716-446655440000/command/ack", topic);
    makapix_topic(MAKAPIX_TOPIC_VIEW_ACK, KEY, topic);
    TEST_ASSERT_EQUAL_STRING("makapix/player/550e8400-e29b-41d4-a716-446655440000/view/ack", topic);
    static const char *const tails[] = {"/command", "/command/ack", "/status", "/capabilities", "/state", "/view", "/view/ack"};
    for (int i = 0; i < MAKAPIX_TOPIC_COUNT; i++) {
        makapix_topic((makapix_topic_t)i, KEY, topic);
        TEST_ASSERT_EQUAL_STRING(tails[i], topic + 15 + 36);
    }
    // A key longer than a key cannot push the topic out of its buffer.
    makapix_topic(MAKAPIX_TOPIC_CAPABILITIES, "550e8400-e29b-41d4-a716-446655440000-and-a-great-deal-more-than-that", topic);
    TEST_ASSERT_TRUE(strlen(topic) < MAKAPIX_TOPIC_LEN);

    makapix_time_text(0, when);
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", when);
    // Back through the reader, at the edges of months and of a leap year.
    static const uint32_t times[] = {946684800u, 1709164800u, 1709251199u, 1709251200u, 1795536378u, 1798761599u, 1798761600u, 4102444800u};
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        makapix_time_text(times[i], when);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(times[i], makapix_time(when), when);
    }
    makapix_time_text(1795536378u, when);
    TEST_ASSERT_EQUAL_STRING("2026-11-24T16:06:18Z", when);
}

static cJSON *object(const char *text)
{
    cJSON *json = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, text);
    return json;
}

static void test_what_the_player_says_is_json_the_server_reads(void)
{
    char out[512];
    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_status(KEY, true, 4028, "2.0.0", 1795536378u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"player_key\":\"550e8400-e29b-41d4-a716-446655440000\",\"status\":\"online\","
                             "\"current_post_id\":4028,\"firmware_version\":\"2.0.0\",\"timestamp\":\"2026-11-24T16:06:18Z\"}", out);
    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_status(KEY, true, 0, "2.0.0-dev", 0, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"current_post_id\":null,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"timestamp\":\"1970-01-01T00:00:00Z\""));
    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_status(KEY, false, 4028, "2.0.0", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"player_key\":\"550e8400-e29b-41d4-a716-446655440000\",\"status\":\"offline\"}", out);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_msg_status(KEY, true, 1, "2.0.0", 5, out, 60));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, makapix_msg_status("a\"b", true, 1, "2.0.0", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, makapix_msg_status(KEY, true, 1, "2.0\\", 5, out, sizeof(out)));

    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_capabilities("2.0.0", out, sizeof(out)));
    cJSON *caps = object(out);
    const cJSON *features = cJSON_GetObjectItem(caps, "features");
    TEST_ASSERT_TRUE(cJSON_IsObject(cJSON_GetObjectItem(features, "pause")));
    TEST_ASSERT_EQUAL(100, cJSON_GetObjectItem(cJSON_GetObjectItem(features, "brightness"), "max")->valueint);
    TEST_ASSERT_EQUAL(4, cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(features, "rotation"), "values")));
    TEST_ASSERT_NULL_MESSAGE(cJSON_GetObjectItem(features, "mirror"), "not declared: the device has none");
    cJSON_Delete(caps);

    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_state(true, 40, 270, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"is_paused\":true,\"brightness\":40,\"rotation\":270}", out);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_msg_state(false, 100, 0, out, 20));

    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_ack("c0ffee", "ok", NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"command_id\":\"c0ffee\",\"status\":\"ok\",\"error\":null}", out);
    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_ack("an \"id\"", "error", "missing or invalid 'value' field\n", out, sizeof(out)));
    cJSON *ack = object(out);
    TEST_ASSERT_EQUAL_STRING("an \"id\"", cJSON_GetObjectItem(ack, "command_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("missing or invalid 'value' field\n", cJSON_GetObjectItem(ack, "error")->valuestring);
    cJSON_Delete(ack);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_msg_ack("c0ffee", "ok", NULL, out, 20));

    const makapix_view_t in_channel = {.post_id = 4028, .random = true, .channel = "by_user", .user_sqid = "QVJ"};
    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_view(KEY, &in_channel, 1795536378u, out, sizeof(out)));
    cJSON *view = object(out);
    TEST_ASSERT_EQUAL(4028, cJSON_GetObjectItem(view, "post_id")->valueint);
    TEST_ASSERT_EQUAL_STRING("2026-11-24T16:06:18Z", cJSON_GetObjectItem(view, "timestamp")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(view, "timezone")->valuestring);
    TEST_ASSERT_EQUAL_STRING("channel", cJSON_GetObjectItem(view, "intent")->valuestring);
    TEST_ASSERT_EQUAL(2, cJSON_GetObjectItem(view, "play_order")->valueint);
    TEST_ASSERT_EQUAL_STRING("by_user", cJSON_GetObjectItem(view, "channel")->valuestring);
    TEST_ASSERT_EQUAL_STRING(KEY, cJSON_GetObjectItem(view, "player_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("QVJ", cJSON_GetObjectItem(view, "channel_user_sqid")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(view, "channel_hashtag")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(view, "request_ack")));
    cJSON_Delete(view);
    const makapix_view_t asked = {.post_id = 7, .asked_for = true, .channel = "hashtag", .hashtag = "cats", .want_ack = true};
    TEST_ASSERT_EQUAL(ESP_OK, makapix_msg_view(KEY, &asked, 0, out, sizeof(out)));
    view = object(out);
    TEST_ASSERT_EQUAL_STRING("artwork", cJSON_GetObjectItem(view, "intent")->valuestring);
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(view, "play_order")->valueint);
    TEST_ASSERT_EQUAL_STRING("cats", cJSON_GetObjectItem(view, "channel_hashtag")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(view, "request_ack")));
    cJSON_Delete(view);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, makapix_msg_view(KEY, &asked, 0, out, 100));
}

static void test_a_command_is_read_from_its_envelope(void)
{
    makapix_command_t c;
    static const char full[] = "{\"command_id\":\"7c9e6679-7425-40de-944b-e07fc1f90ae7\",\"command_type\":\"set_brightness\","
                               "\"payload\":{\"value\":40},\"timestamp\":\"2026-11-25T20:15:00+00:00\"}";
    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_command(full, strlen(full), &c));
    TEST_ASSERT_EQUAL_STRING("7c9e6679-7425-40de-944b-e07fc1f90ae7", c.id);
    TEST_ASSERT_EQUAL_STRING("set_brightness", c.type);
    TEST_ASSERT_EQUAL(40, cJSON_GetObjectItem(c.payload, "value")->valueint);
    cJSON_Delete(c.payload);
    // The bytes are a message, not a string: nothing past `len` is read.
    static const char unterminated[] = "{\"command_type\":\"swap_next\"}{{{{garbage";
    TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_command(unterminated, 28, &c));
    TEST_ASSERT_EQUAL_STRING("swap_next", c.type);
    TEST_ASSERT_EQUAL_STRING("", c.id);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsObject(c.payload) && cJSON_GetArraySize(c.payload) == 0, "no payload is an empty one");
    cJSON_Delete(c.payload);
    static const char *const odd_payloads[] = {"{\"command_type\":\"swap_next\",\"payload\":null}",
                                               "{\"command_type\":\"swap_next\",\"payload\":[1,2]}",
                                               "{\"command_type\":\"swap_next\",\"payload\":\"x\",\"command_id\":null}"};
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, makapix_parse_command(odd_payloads[i], strlen(odd_payloads[i]), &c));
        TEST_ASSERT_TRUE(cJSON_IsObject(c.payload));
        cJSON_Delete(c.payload);
    }
    static const char *const bad[] = {
        "",
        "nonsense",
        "[]",
        "{}",
        "{\"command_type\":7}",
        "{\"command_type\":\"\"}",
        "{\"command_type\":\"a_command_type_far_longer_than_any_the_server_has\"}",
        "{\"command_type\":\"swap_next\",\"command_id\":7}",
        "{\"command_type\":\"swap_next\",\"command_id\":\"an-id-that-is-longer-than-forty-seven-characters-long\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(P3A_ERR_CORRUPT, makapix_parse_command(bad[i], strlen(bad[i]), &c), bad[i]);
        TEST_ASSERT_NULL(c.payload);
    }
    // Cut and mutated envelopes, for the sanitizers.
    char copy[sizeof(full)];
    uint32_t seed = 99;
    for (size_t cut = 0; cut < sizeof(full); cut++) {
        if (makapix_parse_command(full, cut, &c) == ESP_OK) {
            cJSON_Delete(c.payload);
        }
    }
    for (int round = 0; round < 2000; round++) {
        memcpy(copy, full, sizeof(full));
        for (int k = 0; k < 1 + round % 4; k++) {
            seed = seed * 1664525u + 1013904223u;
            copy[(seed >> 8) % (sizeof(full) - 1)] = (char)(1 + (seed >> 16) % 255);
        }
        if (makapix_parse_command(copy, sizeof(full) - 1, &c) == ESP_OK) {
            TEST_ASSERT_TRUE(strlen(c.type) < sizeof(c.type) && strlen(c.id) < sizeof(c.id));
            cJSON_Delete(c.payload);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_waits_grow_to_two_minutes_and_are_spread);
    RUN_TEST(test_three_failed_handshakes_call_for_one_renewal_then_the_registration_is_invalid);
    RUN_TEST(test_a_renewal_is_due_late_in_a_certificates_life_and_never_without_a_clock);
    RUN_TEST(test_a_view_is_due_after_five_seconds_seen_and_every_thirty_after);
    RUN_TEST(test_time_covered_does_not_count_towards_a_view);
    RUN_TEST(test_topics_and_times);
    RUN_TEST(test_what_the_player_says_is_json_the_server_reads);
    RUN_TEST(test_a_command_is_read_from_its_envelope);
    return UNITY_END();
}
