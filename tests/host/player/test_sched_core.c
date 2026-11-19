// SPDX-License-Identifier: Apache-2.0
// The scheduler's arithmetic: shares between channels in both selection
// modes, channels with nothing to play, undoing a turn, and picks.
#include <string.h>

#include "sched_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static sched_core_t core;

static void setup(uint32_t count, const uint32_t *weights, const uint32_t *available, uint64_t seed)
{
    sched_core_init(&core, count, seed);
    for (uint32_t i = 0; i < count; i++) {
        sched_core_set_weight(&core, i, weights ? weights[i] : 0);
        sched_core_set_available(&core, i, available ? available[i] : 10);
    }
}

static void count_selections(sched_select_mode_t mode, uint32_t turns, uint32_t *counts)
{
    memset(counts, 0, SCHED_MAX_CHANNELS * sizeof(uint32_t));
    for (uint32_t n = 0; n < turns; n++) {
        const uint32_t chosen = sched_core_select(&core, mode);
        TEST_ASSERT_TRUE(chosen < core.count);
        counts[chosen]++;
    }
}

static void test_round_robin_gives_exact_shares_evenly_spaced(void)
{
    const uint32_t weights[] = {3, 1};
    uint32_t counts[SCHED_MAX_CHANNELS];
    setup(2, weights, NULL, 1);
    count_selections(SCHED_SELECT_SWRR, 4000, counts);
    TEST_ASSERT_EQUAL(3000, counts[0]);
    TEST_ASSERT_EQUAL(1000, counts[1]);
    // Evenly spaced: the light channel never waits more than four turns.
    uint32_t since = 0;
    for (uint32_t n = 0; n < 400; n++) {
        since = sched_core_select(&core, SCHED_SELECT_SWRR) == 1 ? 0 : since + 1;
        TEST_ASSERT_TRUE(since <= 4);
    }
}

static void test_no_weights_means_equal_shares(void)
{
    uint32_t counts[SCHED_MAX_CHANNELS];
    setup(5, NULL, NULL, 2);
    count_selections(SCHED_SELECT_SWRR, 5000, counts);
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT_UINT32_WITHIN(5, 1000, counts[i]);  // 65536 / 5 does not divide evenly
    }
}

static void test_random_selection_follows_the_weights(void)
{
    const uint32_t weights[] = {50, 30, 20};
    uint32_t counts[SCHED_MAX_CHANNELS];
    setup(3, weights, NULL, 3);
    count_selections(SCHED_SELECT_STOCHASTIC, 30000, counts);
    TEST_ASSERT_UINT32_WITHIN(450, 15000, counts[0]);
    TEST_ASSERT_UINT32_WITHIN(450, 9000, counts[1]);
    TEST_ASSERT_UINT32_WITHIN(450, 6000, counts[2]);
    // And it is random: the same channel does come up twice in a row.
    uint32_t repeats = 0, last = SCHED_NONE;
    for (uint32_t n = 0; n < 1000; n++) {
        const uint32_t chosen = sched_core_select(&core, SCHED_SELECT_STOCHASTIC);
        repeats += chosen == last;
        last = chosen;
    }
    TEST_ASSERT_TRUE(repeats > 100);
}

static void test_a_channel_with_nothing_to_play_is_never_chosen(void)
{
    const uint32_t weights[] = {10, 80, 10};
    const uint32_t available[] = {5, 0, 5};
    uint32_t counts[SCHED_MAX_CHANNELS];
    for (int mode = 0; mode < 2; mode++) {
        setup(3, weights, available, 4);
        count_selections((sched_select_mode_t)mode, 2000, counts);
        TEST_ASSERT_EQUAL(0, counts[1]);
        TEST_ASSERT_UINT32_WITHIN(mode == SCHED_SELECT_SWRR ? 1 : 120, 1000, counts[0]);
        // Its first artwork arrives: it takes its share from then on, and is
        // not owed the turns it missed.
        sched_core_set_available(&core, 1, 1);
        count_selections((sched_select_mode_t)mode, 2000, counts);
        TEST_ASSERT_UINT32_WITHIN(mode == SCHED_SELECT_SWRR ? 2 : 120, 1600, counts[1]);
        // And when it runs dry again it drops out at once.
        sched_core_set_available(&core, 1, 0);
        count_selections((sched_select_mode_t)mode, 500, counts);
        TEST_ASSERT_EQUAL(0, counts[1]);
    }
}

static void test_nothing_anywhere_selects_nothing(void)
{
    const uint32_t available[] = {0, 0};
    setup(2, NULL, available, 5);
    TEST_ASSERT_EQUAL(SCHED_NONE, sched_core_select(&core, SCHED_SELECT_SWRR));
    TEST_ASSERT_EQUAL(SCHED_NONE, sched_core_select(&core, SCHED_SELECT_STOCHASTIC));
    TEST_ASSERT_EQUAL(SCHED_NONE, sched_core_pick(&core, 0, SCHED_PICK_RANDOM));
    TEST_ASSERT_EQUAL(SCHED_NONE, sched_core_pick(&core, 0, SCHED_PICK_RECENCY));
    TEST_ASSERT_EQUAL(SCHED_NONE, sched_core_pick(&core, 9, SCHED_PICK_RANDOM));  // no such channel
    sched_core_init(&core, 0, 5);
    TEST_ASSERT_EQUAL(SCHED_NONE, sched_core_select(&core, SCHED_SELECT_SWRR));
    sched_core_init(&core, 1000, 5);  // more than there is room for
    TEST_ASSERT_EQUAL(SCHED_MAX_CHANNELS, core.count);
}

static void test_an_undone_turn_leaves_the_shares_as_they_were(void)
{
    const uint32_t weights[] = {2, 1, 1};
    uint32_t counts[SCHED_MAX_CHANNELS];
    setup(3, weights, NULL, 6);
    // Every third turn fails to show anything and is undone.
    memset(counts, 0, sizeof(counts));
    uint32_t undone = 0;
    for (uint32_t n = 0; n < 6000; n++) {
        const uint32_t chosen = sched_core_select(&core, SCHED_SELECT_SWRR);
        if (n % 3 == 2) {
            sched_core_undo_select(&core);
            sched_core_undo_select(&core);  // twice is once
            undone++;
        } else {
            counts[chosen]++;
        }
    }
    TEST_ASSERT_EQUAL(2000, undone);
    TEST_ASSERT_EQUAL(2000, counts[0]);
    TEST_ASSERT_EQUAL(1000, counts[1]);
    TEST_ASSERT_EQUAL(1000, counts[2]);
    // With nothing to undo, undoing changes nothing.
    sched_core_t before = core;
    sched_core_undo_select(&core);
    TEST_ASSERT_EQUAL_MEMORY(&before, &core, sizeof(core));
}

static void test_recency_walks_the_channel_in_order_and_wraps(void)
{
    const uint32_t available[] = {4};
    setup(1, NULL, available, 7);
    const uint32_t expect[] = {0, 1, 2, 3, 0, 1, 2, 3, 0};
    for (uint32_t n = 0; n < 9; n++) {
        TEST_ASSERT_EQUAL(expect[n], sched_core_pick(&core, 0, SCHED_PICK_RECENCY));
    }
    // The channel shrinks below the cursor: the walk starts again at the newest.
    sched_core_pick(&core, 0, SCHED_PICK_RECENCY);  // 1; the cursor is at 2
    sched_core_set_available(&core, 0, 2);
    TEST_ASSERT_EQUAL(0, sched_core_pick(&core, 0, SCHED_PICK_RECENCY));
    TEST_ASSERT_EQUAL(1, sched_core_pick(&core, 0, SCHED_PICK_RECENCY));
    TEST_ASSERT_EQUAL(0, sched_core_pick(&core, 0, SCHED_PICK_RECENCY));
}

static void test_random_picks_stay_in_range_and_reach_everything(void)
{
    const uint32_t available[] = {37, 1};
    uint32_t seen[37] = {0};
    setup(2, NULL, available, 8);
    for (uint32_t n = 0; n < 3700; n++) {
        const uint32_t at = sched_core_pick(&core, 0, SCHED_PICK_RANDOM);
        TEST_ASSERT_TRUE(at < 37);
        seen[at]++;
        TEST_ASSERT_EQUAL(0, sched_core_pick(&core, 1, SCHED_PICK_RANDOM));  // one artwork: always that one
    }
    for (uint32_t i = 0; i < 37; i++) {
        TEST_ASSERT_UINT32_WITHIN(50, 100, seen[i]);
    }
}

static void test_the_same_seed_gives_the_same_choices_and_another_seed_others(void)
{
    uint32_t first[200], second[200];
    uint32_t differing = 0;
    for (int run = 0; run < 3; run++) {
        setup(4, NULL, NULL, run == 2 ? 99 : 42);
        for (uint32_t n = 0; n < 200; n++) {
            const uint32_t ch = sched_core_select(&core, SCHED_SELECT_STOCHASTIC);
            const uint32_t value = ch * 100 + sched_core_pick(&core, ch, SCHED_PICK_RANDOM);
            if (run == 0) {
                first[n] = value;
            } else if (run == 1) {
                second[n] = value;
            } else {
                differing += value != first[n];
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32_ARRAY(first, second, 200);
    TEST_ASSERT_TRUE(differing > 150);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_robin_gives_exact_shares_evenly_spaced);
    RUN_TEST(test_no_weights_means_equal_shares);
    RUN_TEST(test_random_selection_follows_the_weights);
    RUN_TEST(test_a_channel_with_nothing_to_play_is_never_chosen);
    RUN_TEST(test_nothing_anywhere_selects_nothing);
    RUN_TEST(test_an_undone_turn_leaves_the_shares_as_they_were);
    RUN_TEST(test_recency_walks_the_channel_in_order_and_wraps);
    RUN_TEST(test_random_picks_stay_in_range_and_reach_everything);
    RUN_TEST(test_the_same_seed_gives_the_same_choices_and_another_seed_others);
    return UNITY_END();
}
