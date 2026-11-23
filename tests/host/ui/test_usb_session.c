// SPDX-License-Identifier: Apache-2.0
// A USB session with the card: when it begins, when it is over, and that
// it always ends in a restart.
#include "unity.h"
#include "usb_session.h"

void setUp(void) {}
void tearDown(void) {}

static usb_session_t s;

static void test_plugging_in_takes_the_card_once_and_unplugging_ends_in_a_restart(void)
{
    usb_session_init(&s);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 1000, false));  // nothing plugged in: nothing ever
    TEST_ASSERT_FALSE(s.taken);

    TEST_ASSERT_EQUAL(USB_DO_TAKE_CARD, usb_session_mounted(&s, 5000));
    TEST_ASSERT_TRUE(s.taken && s.active);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 999000, true));  // for as long as it stays in

    usb_session_unmounted(&s, 1000000);
    TEST_ASSERT_FALSE(s.active);  // no more reads or writes for the computer
    TEST_ASSERT_TRUE(s.taken);    // and the card does not go back to the device
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 1001999, false));
    TEST_ASSERT_EQUAL(USB_DO_ANNOUNCE_RESTART, usb_session_tick(&s, 1002000, false));
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 1005999, false));
    TEST_ASSERT_EQUAL(USB_DO_RESTART, usb_session_tick(&s, 1006000, false));
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 1006250, false));  // asked for once
}

static void test_a_bounce_on_the_way_out_does_not_start_a_new_session(void)
{
    usb_session_init(&s);
    usb_session_mounted(&s, 0);
    usb_session_unmounted(&s, 10000);
    // "Configured" again 300 ms later, as a cable being pulled does.
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_mounted(&s, 10300));
    TEST_ASSERT_FALSE(s.active);
    // At the recheck the computer is gone: the restart goes ahead.
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 11700, false));
    TEST_ASSERT_EQUAL(USB_DO_ANNOUNCE_RESTART, usb_session_tick(&s, 12000, false));
}

static void test_a_quick_replug_continues_the_session_without_a_restart(void)
{
    usb_session_init(&s);
    usb_session_mounted(&s, 0);
    usb_session_unmounted(&s, 10000);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_mounted(&s, 10300));  // suspect, not taken yet
    // Still configured when looked at again: it was a real mount.
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 11700, true));  // nothing new to take: it was taken already
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 60000, true));  // and no restart while it stays
    // A mount well after the window is taken at once.
    usb_session_unmounted(&s, 70000);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_mounted(&s, 71600));
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 80000, true));
}

static void test_a_short_suspend_is_ridden_out_and_a_long_one_ends_the_session(void)
{
    usb_session_init(&s);
    usb_session_mounted(&s, 0);
    usb_session_suspended(&s, 10000);
    TEST_ASSERT_FALSE(s.active);
    usb_session_resumed(&s, true);
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 20000, true));

    // The laptop goes to sleep with the cable in.
    usb_session_suspended(&s, 30000);
    TEST_ASSERT_EQUAL(USB_DO_ANNOUNCE_RESTART, usb_session_tick(&s, 32000, true));
    // Waking up now is too late.
    usb_session_resumed(&s, true);
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_mounted(&s, 33000));
    TEST_ASSERT_EQUAL(USB_DO_RESTART, usb_session_tick(&s, 36000, true));
}

static void test_a_suspend_while_plugging_in_does_not_make_the_mount_a_bounce(void)
{
    usb_session_init(&s);
    usb_session_suspended(&s, 100);  // the bus idles before the computer configures the device
    TEST_ASSERT_EQUAL(USB_DO_NOTHING, usb_session_tick(&s, 5000, false));  // no session yet: nothing to end
    TEST_ASSERT_EQUAL(USB_DO_TAKE_CARD, usb_session_mounted(&s, 5100));
    TEST_ASSERT_TRUE(s.active);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plugging_in_takes_the_card_once_and_unplugging_ends_in_a_restart);
    RUN_TEST(test_a_bounce_on_the_way_out_does_not_start_a_new_session);
    RUN_TEST(test_a_quick_replug_continues_the_session_without_a_restart);
    RUN_TEST(test_a_short_suspend_is_ridden_out_and_a_long_one_ends_the_session);
    RUN_TEST(test_a_suspend_while_plugging_in_does_not_make_the_mount_a_bounce);
    return UNITY_END();
}
