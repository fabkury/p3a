// SPDX-License-Identifier: Apache-2.0
// When the device looks for an update, and when a newly installed firmware
// counts as good.
#include "unity.h"
#include "update_policy.h"

void setUp(void) {}
void tearDown(void) {}

static update_clock_t ready(uint32_t now_s)
{
    return (update_clock_t){.enabled = true, .net_up = true, .net_seen = true, .net_up_s = 30, .now_s = now_s};
}

static void test_the_first_look_is_two_minutes_after_the_network_comes_up(void)
{
    update_clock_t c = ready(30);
    TEST_ASSERT_FALSE(update_check_due(&c));
    c.now_s = 30 + UPDATE_FIRST_CHECK_S - 1;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c.now_s = 30 + UPDATE_FIRST_CHECK_S;
    TEST_ASSERT_TRUE(update_check_due(&c));
    // Counted from when the network came up, not from boot: an hour in setup changes nothing.
    c = ready(3600 + 10);
    c.net_up_s = 3600;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c.now_s = 3600 + UPDATE_FIRST_CHECK_S;
    TEST_ASSERT_TRUE(update_check_due(&c));
}

static void test_nothing_is_looked_for_when_it_should_not_or_cannot_be(void)
{
    update_clock_t c = ready(100000);
    TEST_ASSERT_TRUE(update_check_due(&c));
    c.enabled = false;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c = ready(100000);
    c.net_up = false;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c = ready(100000);
    c.busy = true;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c = ready(100000);
    c.net_seen = false;
    TEST_ASSERT_FALSE(update_check_due(&c));
    TEST_ASSERT_FALSE(update_check_due(NULL));
}

static void test_after_an_answer_it_looks_again_in_twelve_hours_and_after_none_in_one(void)
{
    update_clock_t c = ready(1000);
    c.checked = true;
    c.last_ok = true;
    c.last_s = 1000;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c.now_s = 1000 + UPDATE_CHECK_EVERY_S - 1;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c.now_s = 1000 + UPDATE_CHECK_EVERY_S;
    TEST_ASSERT_TRUE(update_check_due(&c));

    c.last_ok = false;
    c.now_s = 1000 + UPDATE_RETRY_S - 1;
    TEST_ASSERT_FALSE(update_check_due(&c));
    c.now_s = 1000 + UPDATE_RETRY_S;
    TEST_ASSERT_TRUE(update_check_due(&c));
}

static void test_a_new_firmware_is_good_once_its_owner_can_reach_it(void)
{
    update_trial_facts_t f = {.uptime_s = 20, .boot_done = true, .has_credentials = true, .net_up = true, .http_up = true};
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_GOOD, update_trial_verdict(&f));
    // Each thing missing keeps it waiting.
    f.net_up = false;
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_WAIT, update_trial_verdict(&f));
    f.net_up = true;
    f.http_up = false;
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_WAIT, update_trial_verdict(&f));
    f.http_up = true;
    f.boot_done = false;
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_WAIT, update_trial_verdict(&f));
    // A device with no stored network is reached on its own: the setup page being up is enough.
    f = (update_trial_facts_t){.uptime_s = 20, .boot_done = true, .has_credentials = false, .http_up = true};
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_GOOD, update_trial_verdict(&f));
    f.http_up = false;
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_WAIT, update_trial_verdict(&f));
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_WAIT, update_trial_verdict(NULL));
}

static void test_a_new_firmware_that_stays_unreachable_is_given_up_after_ten_minutes(void)
{
    update_trial_facts_t f = {.uptime_s = UPDATE_TRIAL_LIMIT_S - 1, .boot_done = true, .has_credentials = true, .http_up = true};
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_WAIT, update_trial_verdict(&f));
    f.uptime_s = UPDATE_TRIAL_LIMIT_S;
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_BAD, update_trial_verdict(&f));
    // Reached at the last moment is still reached.
    f.net_up = true;
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_GOOD, update_trial_verdict(&f));
    // One that never finished booting is given up too.
    f = (update_trial_facts_t){.uptime_s = UPDATE_TRIAL_LIMIT_S + 5};
    TEST_ASSERT_EQUAL(UPDATE_TRIAL_BAD, update_trial_verdict(&f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_look_is_two_minutes_after_the_network_comes_up);
    RUN_TEST(test_nothing_is_looked_for_when_it_should_not_or_cannot_be);
    RUN_TEST(test_after_an_answer_it_looks_again_in_twelve_hours_and_after_none_in_one);
    RUN_TEST(test_a_new_firmware_is_good_once_its_owner_can_reach_it);
    RUN_TEST(test_a_new_firmware_that_stays_unreachable_is_given_up_after_ten_minutes);
    return UNITY_END();
}
