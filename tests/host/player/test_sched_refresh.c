// SPDX-License-Identifier: Apache-2.0
// When a channel is listed again, and the waits after a listing that failed.
#include "sched_refresh.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static sched_refresh_t r;

#define HOUR_MS 3600000LL

static void test_a_new_channel_is_due_at_once_and_then_at_its_interval(void)
{
    sched_refresh_init(&r, 3600, true);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 0, false));  // it needs the network
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 0, true));
    sched_refresh_started(&r);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 0, true));  // not twice at once
    sched_refresh_finished(&r, SCHED_LISTED, 0, 5000);
    TEST_ASSERT_TRUE(r.ever_listed);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 5000 + HOUR_MS - 1, true));
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 5000 + HOUR_MS, true));
    TEST_ASSERT_EQUAL(3600, sched_refresh_wait_s(&r, 5000));
    TEST_ASSERT_EQUAL(0, sched_refresh_wait_s(&r, 5000 + HOUR_MS));
}

static void test_a_source_without_an_interval_is_listed_once_and_then_only_when_asked(void)
{
    sched_refresh_init(&r, 0, false);
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 0, false));  // the user's files need no network
    sched_refresh_started(&r);
    sched_refresh_finished(&r, SCHED_LISTED, 0, 1000);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 1000 + 1000 * HOUR_MS, false));
    TEST_ASSERT_EQUAL(-1, sched_refresh_wait_s(&r, 2000));
    sched_refresh_ask(&r, 9000);  // an upload
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 9000, false));
}

static void test_an_index_found_on_the_card_waits_out_the_rest_of_its_interval(void)
{
    sched_refresh_init(&r, 3600, true);
    sched_refresh_known(&r, 600, 10000);  // written ten minutes ago
    TEST_ASSERT_TRUE(r.ever_listed);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 10000, true));
    TEST_ASSERT_EQUAL(3000, sched_refresh_wait_s(&r, 10000));
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 10000 + 3000 * 1000LL, true));

    sched_refresh_init(&r, 3600, true);
    sched_refresh_known(&r, 99999, 10000);  // older than the interval
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 10000, true));
    sched_refresh_init(&r, 3600, true);
    sched_refresh_known(&r, -1, 10000);  // the clock is not set: its age is unknown
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 10000, true));
    sched_refresh_init(&r, 0, false);
    sched_refresh_known(&r, 5, 10000);  // a source listed only when asked
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 99999999, true));
}

static void finish(sched_list_result_t result, uint32_t retry_after, int64_t now)
{
    sched_refresh_started(&r);
    sched_refresh_finished(&r, result, retry_after, now);
}

static void test_failures_wait_a_minute_then_double_up_to_an_hour(void)
{
    sched_refresh_init(&r, 3600, true);
    const int64_t waits[] = {60, 120, 240, 480, 960, 1920, 3600, 3600, 3600};
    int64_t now = 0;
    for (unsigned i = 0; i < 9; i++) {
        finish(SCHED_LIST_FAILED, 0, now);
        TEST_ASSERT_EQUAL_INT64(waits[i], sched_refresh_wait_s(&r, now));
        TEST_ASSERT_FALSE(sched_refresh_due(&r, now + waits[i] * 1000 - 1, true));
        now += waits[i] * 1000;
        TEST_ASSERT_TRUE(sched_refresh_due(&r, now, true));
    }
    TEST_ASSERT_FALSE(r.ever_listed);
    // One success and the count starts over.
    finish(SCHED_LISTED, 0, now);
    now += HOUR_MS;
    finish(SCHED_LIST_FAILED, 0, now);
    TEST_ASSERT_EQUAL_INT64(60, sched_refresh_wait_s(&r, now));
}

static void test_a_rate_limit_waits_what_the_server_asked_and_never_less_than_a_quarter_hour(void)
{
    sched_refresh_init(&r, 3600, true);
    finish(SCHED_LIST_RATE_LIMITED, 0, 0);
    TEST_ASSERT_EQUAL_INT64(900, sched_refresh_wait_s(&r, 0));
    finish(SCHED_LIST_RATE_LIMITED, 30, 0);  // "retry in 30 s" from a server already refusing
    TEST_ASSERT_EQUAL_INT64(900, sched_refresh_wait_s(&r, 0));
    finish(SCHED_LIST_RATE_LIMITED, 7200, 0);
    TEST_ASSERT_EQUAL_INT64(7200, sched_refresh_wait_s(&r, 0));
}

static void test_a_missing_key_waits_an_hour_or_until_asked(void)
{
    sched_refresh_init(&r, 600, true);
    finish(SCHED_LIST_NO_KEY, 0, 0);
    TEST_ASSERT_EQUAL_INT64(3600, sched_refresh_wait_s(&r, 0));
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 600 * 1000, true));  // not at the source's interval
    sched_refresh_ask(&r, 1000);  // the key was entered
    TEST_ASSERT_TRUE(sched_refresh_due(&r, 1000, true));
}

static void test_a_failed_card_stops_it_for_good_and_asking_while_listing_changes_nothing(void)
{
    sched_refresh_init(&r, 600, false);
    finish(SCHED_LIST_CARD, 0, 0);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 999999999, true));
    TEST_ASSERT_EQUAL(-1, sched_refresh_wait_s(&r, 0));
    sched_refresh_ask(&r, 5000);
    TEST_ASSERT_FALSE(sched_refresh_due(&r, 999999999, true));

    sched_refresh_init(&r, 600, false);
    sched_refresh_started(&r);
    sched_refresh_ask(&r, 100);
    TEST_ASSERT_EQUAL(SCHED_REFRESH_RUNNING, r.state);
    TEST_ASSERT_EQUAL(0, sched_refresh_wait_s(&r, 100));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_new_channel_is_due_at_once_and_then_at_its_interval);
    RUN_TEST(test_a_source_without_an_interval_is_listed_once_and_then_only_when_asked);
    RUN_TEST(test_an_index_found_on_the_card_waits_out_the_rest_of_its_interval);
    RUN_TEST(test_failures_wait_a_minute_then_double_up_to_an_hour);
    RUN_TEST(test_a_rate_limit_waits_what_the_server_asked_and_never_less_than_a_quarter_hour);
    RUN_TEST(test_a_missing_key_waits_an_hour_or_until_asked);
    RUN_TEST(test_a_failed_card_stops_it_for_good_and_asking_while_listing_changes_nothing);
    return UNITY_END();
}
