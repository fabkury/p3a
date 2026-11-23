// SPDX-License-Identifier: Apache-2.0
// The pick within a channel: less repetition (change C) and the filter for
// elongated artworks (change D).
#include <string.h>

#include "sched_pick.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define N_MAX 2000

static sched_core_t core;
static sched_recent_t recent;
static uint32_t widths[N_MAX], heights[N_MAX];

// Artwork n of the test channel is called 1000 + n.
static bool describe(uint32_t ordinal, sched_candidate_t *out, void *ctx)
{
    const uint32_t count = *(const uint32_t *)ctx;
    if (ordinal >= count) {
        return false;
    }
    out->id = 1000 + ordinal;
    out->width = widths[ordinal];
    out->height = heights[ordinal];
    return true;
}

static void channel_of(uint32_t count, uint64_t seed)
{
    sched_core_init(&core, 1, seed);
    sched_core_set_weight(&core, 0, 1);
    sched_core_set_available(&core, 0, count);
    sched_recent_clear(&recent);
    memset(widths, 0, sizeof(widths));
    memset(heights, 0, sizeof(heights));
}

static uint32_t play(uint32_t count, sched_pick_mode_t mode, uint32_t max_tenths)
{
    uint32_t n = count;
    const uint32_t ordinal = sched_pick_artwork(&core, 0, mode, &recent, max_tenths, describe, &n);
    if (ordinal != SCHED_NONE) {
        sched_recent_add(&recent, 1000 + ordinal);
    }
    return ordinal;
}

static void test_the_list_of_what_was_shown(void)
{
    sched_recent_clear(&recent);
    TEST_ASSERT_EQUAL_UINT32(SCHED_NONE, sched_recent_ago(&recent, 7));
    sched_recent_add(&recent, 7);
    sched_recent_add(&recent, 8);
    TEST_ASSERT_EQUAL(0, sched_recent_ago(&recent, 8));
    TEST_ASSERT_EQUAL(1, sched_recent_ago(&recent, 7));
    sched_recent_add(&recent, 7);
    TEST_ASSERT_EQUAL(0, sched_recent_ago(&recent, 7));  // the latest showing counts
    for (uint32_t i = 0; i < SCHED_RECENT_MAX; i++) {
        sched_recent_add(&recent, 100 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(SCHED_NONE, sched_recent_ago(&recent, 7));  // pushed out
    TEST_ASSERT_EQUAL(SCHED_RECENT_MAX - 1, sched_recent_ago(&recent, 100));
}

static void test_the_chance_grows_with_time_and_never_reaches_zero(void)
{
    TEST_ASSERT_EQUAL(65536, sched_recent_chance(SCHED_NONE, 50));
    TEST_ASSERT_EQUAL(SCHED_PICK_FLOOR, sched_recent_chance(0, 50));
    TEST_ASSERT_TRUE(sched_recent_chance(10, 50) > sched_recent_chance(5, 50));
    TEST_ASSERT_EQUAL(65536, sched_recent_chance(50, 50));
    TEST_ASSERT_EQUAL(65536, sched_recent_chance(300, 5000));  // beyond what is remembered
    TEST_ASSERT_TRUE(sched_recent_chance(0, 1) > 0);
}

static void test_no_artwork_plays_twice_in_a_row(void)
{
    for (uint32_t count = 2; count <= 5; count++) {
        channel_of(count, 42 + count);
        uint32_t last = SCHED_NONE;
        for (int i = 0; i < 5000; i++) {
            const uint32_t got = play(count, SCHED_PICK_RANDOM, 0);
            TEST_ASSERT_TRUE(got < count);
            TEST_ASSERT_NOT_EQUAL(last, got);
            last = got;
        }
    }
    // A channel of one has no choice.
    channel_of(1, 9);
    TEST_ASSERT_EQUAL(0, play(1, SCHED_PICK_RANDOM, 0));
    TEST_ASSERT_EQUAL(0, play(1, SCHED_PICK_RANDOM, 0));
    channel_of(0, 9);
    TEST_ASSERT_EQUAL_UINT32(SCHED_NONE, play(0, SCHED_PICK_RANDOM, 0));
}

// The measure of repetition: how many plays lie between two showings of the
// same artwork. A uniform pick over n gives gaps shorter than n / 4 about
// 22% of the time; with the list it measures 1.5% to 4.3%.
static void test_artworks_do_not_come_back_soon_and_all_get_their_share(void)
{
    const uint32_t counts[] = {12, 60, 400};
    for (unsigned c = 0; c < 3; c++) {
        const uint32_t count = counts[c];
        channel_of(count, 1234 + c);
        static uint32_t last_at[N_MAX], shown[N_MAX];
        memset(last_at, 0, sizeof(last_at));
        memset(shown, 0, sizeof(shown));
        const uint32_t plays = count * 200;
        uint32_t soon = 0, repeats = 0;
        for (uint32_t i = 1; i <= plays; i++) {
            const uint32_t got = play(count, SCHED_PICK_RANDOM, 0);
            TEST_ASSERT_TRUE(got < count);
            if (last_at[got] != 0) {
                repeats++;
                if (i - last_at[got] < count / 4) {
                    soon++;
                }
            }
            last_at[got] = i;
            shown[got]++;
        }
        TEST_ASSERT_TRUE_MESSAGE(soon * 100 < repeats * 6, "under 6% of repeats come within a quarter of the channel");
        for (uint32_t i = 0; i < count; i++) {
            // Each artwork's share is within a third of the even share of 200.
            TEST_ASSERT_UINT32_WITHIN(67, 200, shown[i]);
        }
    }
}

static void test_recency_order_is_untouched_by_the_list(void)
{
    channel_of(4, 5);
    for (uint32_t i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL(i % 4, play(4, SCHED_PICK_RECENCY, 0));
    }
}

static void test_what_counts_as_too_elongated(void)
{
    TEST_ASSERT_FALSE(sched_aspect_too_long(4000, 1000, 40));  // exactly at the limit passes
    TEST_ASSERT_TRUE(sched_aspect_too_long(4001, 1000, 40));
    TEST_ASSERT_TRUE(sched_aspect_too_long(1000, 4001, 40));   // tall counts like wide
    TEST_ASSERT_FALSE(sched_aspect_too_long(720, 720, 25));
    TEST_ASSERT_FALSE(sched_aspect_too_long(0, 9000, 25));     // not measured yet: let through
    TEST_ASSERT_FALSE(sched_aspect_too_long(9000, 0, 25));
    TEST_ASSERT_FALSE(sched_aspect_too_long(9000, 1, 0));      // no filter
    TEST_ASSERT_TRUE(sched_aspect_too_long(65535, 1, 100));
    TEST_ASSERT_FALSE(sched_aspect_too_long(251, 100, 26));
    TEST_ASSERT_TRUE(sched_aspect_too_long(251, 100, 25));
}

static void test_elongated_artworks_are_passed_over_in_both_orders(void)
{
    channel_of(6, 77);
    for (uint32_t i = 0; i < 6; i++) {
        widths[i] = 1000;
        heights[i] = 1000;
    }
    widths[1] = 5000;   // too wide
    heights[4] = 4500;  // too tall
    widths[2] = 0;      // unknown: let through
    for (int i = 0; i < 2000; i++) {
        const uint32_t got = play(6, SCHED_PICK_RANDOM, 40);
        TEST_ASSERT_TRUE(got == 0 || got == 2 || got == 3 || got == 5);
    }
    const uint32_t expected[] = {0, 2, 3, 5};
    core.channels[0].cursor = 0;
    for (uint32_t i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL(expected[i % 4], play(6, SCHED_PICK_RECENCY, 40));
    }
    // With the filter off they play.
    bool seen_wide = false;
    for (int i = 0; i < 200; i++) {
        seen_wide = seen_wide || play(6, SCHED_PICK_RANDOM, 0) == 1;
    }
    TEST_ASSERT_TRUE(seen_wide);
}

static void test_a_channel_of_nothing_but_elongated_artworks_gives_nothing(void)
{
    channel_of(3, 1);
    for (uint32_t i = 0; i < 3; i++) {
        widths[i] = 9000;
        heights[i] = 1000;
    }
    TEST_ASSERT_EQUAL_UINT32(SCHED_NONE, play(3, SCHED_PICK_RANDOM, 40));
    TEST_ASSERT_EQUAL_UINT32(SCHED_NONE, play(3, SCHED_PICK_RECENCY, 40));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_list_of_what_was_shown);
    RUN_TEST(test_the_chance_grows_with_time_and_never_reaches_zero);
    RUN_TEST(test_no_artwork_plays_twice_in_a_row);
    RUN_TEST(test_artworks_do_not_come_back_soon_and_all_get_their_share);
    RUN_TEST(test_recency_order_is_untouched_by_the_list);
    RUN_TEST(test_what_counts_as_too_elongated);
    RUN_TEST(test_elongated_artworks_are_passed_over_in_both_orders);
    RUN_TEST(test_a_channel_of_nothing_but_elongated_artworks_gives_nothing);
    return UNITY_END();
}
