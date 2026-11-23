// SPDX-License-Identifier: Apache-2.0
// The steps of formatting the SD card on the device.
#include <string.h>

#include "ui_card.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ui_card_t c;
static p3a_ui_content_t words;

static bool shown(int64_t now)
{
    return ui_card_content(&c, now, &words);
}

static bool enabled(int i)
{
    return (words.buttons_enabled >> i) & 1;
}

static void test_nothing_is_on_the_screen_until_it_is_offered_or_asked_for(void)
{
    ui_card_init(&c);
    TEST_ASSERT_FALSE(shown(0));
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 0));
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 99999));
    ui_card_long_press(&c);
    ui_card_formatted(&c, true, 0);  // a result nobody asked for
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_probed(&c, UI_CARD_FOUND_GOOD, 0));
    TEST_ASSERT_FALSE(shown(0));
}

static void test_from_the_boot_offer_through_the_warning_to_a_formatted_card(void)
{
    ui_card_init(&c);
    ui_card_offer(&c);
    TEST_ASSERT_TRUE(shown(0));
    TEST_ASSERT_EQUAL_STRING("No usable SD card", words.title);
    TEST_ASSERT_EQUAL_STRING("Format this card", words.buttons[0]);
    TEST_ASSERT_TRUE(enabled(0));
    TEST_ASSERT_EQUAL_STRING("", words.buttons[1]);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 1, 0));  // there is no second button

    // The button only looks: nothing is erased by the first tap.
    TEST_ASSERT_EQUAL(UI_CARD_DO_PROBE, ui_card_button(&c, 0, 1000));
    TEST_ASSERT_TRUE(shown(1000));
    TEST_ASSERT_EQUAL_STRING("", words.buttons[0]);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 1100));  // no double start

    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_probed(&c, UI_CARD_FOUND_UNUSABLE, 2000));
    TEST_ASSERT_TRUE(shown(2000));
    TEST_ASSERT_EQUAL_STRING("Erase the SD card?", words.title);
    TEST_ASSERT_NOT_NULL(strstr(words.text, "cannot be undone"));
    TEST_ASSERT_EQUAL_STRING("Wait 3", words.buttons[0]);
    TEST_ASSERT_FALSE(enabled(0));
    TEST_ASSERT_EQUAL_STRING("Cancel", words.buttons[1]);
    TEST_ASSERT_TRUE(enabled(1));

    // The confirm button is dead for three seconds, however often it is tapped.
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 2000));
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 4999));
    shown(3500);
    TEST_ASSERT_EQUAL_STRING("Wait 2", words.buttons[0]);
    shown(5000);
    TEST_ASSERT_EQUAL_STRING("Erase and format", words.buttons[0]);
    TEST_ASSERT_TRUE(enabled(0));
    TEST_ASSERT_EQUAL(UI_CARD_DO_FORMAT, ui_card_button(&c, 0, 5000));
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 5001));  // once
    shown(5001);
    TEST_ASSERT_EQUAL_STRING("Formatting", words.title);
    TEST_ASSERT_EQUAL_STRING("", words.buttons[0]);
    ui_card_long_press(&c);  // too late to cancel: nothing happens
    TEST_ASSERT_EQUAL(UI_CARD_FORMATTING, c.phase);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 60000));  // however long it takes

    ui_card_formatted(&c, true, 61000);
    shown(61000);
    TEST_ASSERT_EQUAL_STRING("Done", words.title);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 62499));
    TEST_ASSERT_EQUAL(UI_CARD_DO_RESTART, ui_card_tick(&c, 62500));
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 62750));  // asked for once
    TEST_ASSERT_TRUE(shown(62750));  // and the notice stays until the restart
}

static void test_cancelling_the_warning_by_button_or_long_press(void)
{
    // From the boot offer: back to the offer.
    ui_card_init(&c);
    ui_card_offer(&c);
    ui_card_button(&c, 0, 0);
    ui_card_probed(&c, UI_CARD_FOUND_UNUSABLE, 0);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 1, 100));  // cancel works at once
    TEST_ASSERT_EQUAL(UI_CARD_OFFER, c.phase);
    ui_card_button(&c, 0, 0);
    ui_card_probed(&c, UI_CARD_FOUND_UNUSABLE, 0);
    ui_card_long_press(&c);
    TEST_ASSERT_EQUAL(UI_CARD_OFFER, c.phase);

    // From the information screen: the flow leaves the screen.
    ui_card_init(&c);
    ui_card_ask(&c, 1000);
    TEST_ASSERT_EQUAL(UI_CARD_WARNING, c.phase);
    ui_card_ask(&c, 2500);  // asked again while up: the three seconds do not start over
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 3999));
    ui_card_long_press(&c);
    TEST_ASSERT_FALSE(shown(4000));
    // And can be asked for again, with a fresh wait.
    ui_card_ask(&c, 10000);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 12999));
    TEST_ASSERT_EQUAL(UI_CARD_DO_FORMAT, ui_card_button(&c, 0, 13000));
}

static void test_looking_again_finds_no_card_or_a_good_one(void)
{
    ui_card_init(&c);
    ui_card_offer(&c);
    ui_card_button(&c, 0, 0);
    ui_card_probed(&c, UI_CARD_FOUND_NOTHING, 1000);
    shown(1000);
    TEST_ASSERT_EQUAL_STRING("No card found", words.title);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 4999));
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 5000));
    TEST_ASSERT_EQUAL(UI_CARD_OFFER, c.phase);  // back to the offer, to try again

    // A good card was put in meanwhile: nothing is erased, the device restarts.
    ui_card_button(&c, 0, 6000);
    ui_card_probed(&c, UI_CARD_FOUND_GOOD, 7000);
    shown(7000);
    TEST_ASSERT_EQUAL_STRING("The card works", words.title);
    TEST_ASSERT_EQUAL_STRING("", words.buttons[0]);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_button(&c, 0, 7100));
    TEST_ASSERT_EQUAL(UI_CARD_DO_RESTART, ui_card_tick(&c, 8500));
}

static void test_a_failed_format_says_so_and_restarts(void)
{
    ui_card_init(&c);
    ui_card_ask(&c, 0);
    TEST_ASSERT_EQUAL(UI_CARD_DO_FORMAT, ui_card_button(&c, 0, 3000));
    ui_card_formatted(&c, false, 10000);
    shown(10000);
    TEST_ASSERT_EQUAL_STRING("Format failed", words.title);
    TEST_ASSERT_EQUAL_STRING("", words.buttons[0]);
    TEST_ASSERT_EQUAL(UI_CARD_DO_NOTHING, ui_card_tick(&c, 15999));
    TEST_ASSERT_EQUAL(UI_CARD_DO_RESTART, ui_card_tick(&c, 16000));
}

static void test_the_offer_does_not_interrupt_a_flow_under_way(void)
{
    ui_card_init(&c);
    ui_card_ask(&c, 0);
    ui_card_offer(&c);
    TEST_ASSERT_EQUAL(UI_CARD_WARNING, c.phase);
    TEST_ASSERT_FALSE(c.from_boot);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_on_the_screen_until_it_is_offered_or_asked_for);
    RUN_TEST(test_from_the_boot_offer_through_the_warning_to_a_formatted_card);
    RUN_TEST(test_cancelling_the_warning_by_button_or_long_press);
    RUN_TEST(test_looking_again_finds_no_card_or_a_good_one);
    RUN_TEST(test_a_failed_format_says_so_and_restarts);
    RUN_TEST(test_the_offer_does_not_interrupt_a_flow_under_way);
    return UNITY_END();
}
