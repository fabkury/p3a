// SPDX-License-Identifier: Apache-2.0
// The history behind "back" and "forward again".
#include <stdio.h>
#include <string.h>

#include "sched_history.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static sched_history_t h;

static p3a_record_t rec(unsigned n)
{
    p3a_record_t r;
    char name[24];
    snprintf(name, sizeof(name), "f%u.gif", n);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_local(&r, name));
    r.sort_at = n;
    return r;
}

static void add(unsigned n)
{
    p3a_record_t r = rec(n);
    sched_history_add(&h, &r);
}

static void test_back_and_forward_walk_what_was_shown(void)
{
    p3a_record_t out;
    sched_history_clear(&h);
    TEST_ASSERT_FALSE(sched_history_back(&h, &out));
    TEST_ASSERT_FALSE(sched_history_forward(&h, &out));
    add(1);
    TEST_ASSERT_FALSE(sched_history_back(&h, &out));  // nothing before the first
    add(2);
    add(3);
    TEST_ASSERT_TRUE(sched_history_back(&h, &out));
    TEST_ASSERT_EQUAL(2, out.sort_at);
    TEST_ASSERT_TRUE(sched_history_back(&h, &out));
    TEST_ASSERT_EQUAL(1, out.sort_at);
    TEST_ASSERT_FALSE(sched_history_back(&h, &out));  // the far end: stays on 1
    TEST_ASSERT_EQUAL(2, h.position);
    TEST_ASSERT_TRUE(sched_history_forward(&h, &out));
    TEST_ASSERT_EQUAL(2, out.sort_at);
    TEST_ASSERT_TRUE(sched_history_forward(&h, &out));
    TEST_ASSERT_EQUAL(3, out.sort_at);
    TEST_ASSERT_FALSE(sched_history_forward(&h, &out));  // the present: pick something new
}

static void test_a_new_artwork_returns_to_the_present_and_repeats_add_nothing(void)
{
    p3a_record_t out;
    sched_history_clear(&h);
    add(1);
    add(2);
    add(2);  // shown again: not a second entry
    TEST_ASSERT_EQUAL(2, h.count);
    TEST_ASSERT_TRUE(sched_history_back(&h, &out));
    add(3);
    TEST_ASSERT_EQUAL(0, h.position);
    TEST_ASSERT_TRUE(sched_history_back(&h, &out));
    TEST_ASSERT_EQUAL(2, out.sort_at);
}

static void test_only_the_last_32_are_kept(void)
{
    p3a_record_t out;
    sched_history_clear(&h);
    for (unsigned n = 1; n <= 100; n++) {
        add(n);
    }
    TEST_ASSERT_EQUAL(SCHED_HISTORY_SIZE, h.count);
    for (unsigned n = 99; n >= 69; n--) {
        TEST_ASSERT_TRUE(sched_history_back(&h, &out));
        TEST_ASSERT_EQUAL(n, out.sort_at);
    }
    TEST_ASSERT_FALSE(sched_history_back(&h, &out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_back_and_forward_walk_what_was_shown);
    RUN_TEST(test_a_new_artwork_returns_to_the_present_and_repeats_add_nothing);
    RUN_TEST(test_only_the_last_32_are_kept);
    return UNITY_END();
}
