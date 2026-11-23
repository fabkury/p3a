// SPDX-License-Identifier: Apache-2.0
// The model of what is on the screen: precedence, uncovering, notices as
// banner or screen, expiry, the per-boot cap.
#include <stdio.h>
#include <string.h>

#include "ui_model.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ui_model_t m;

static p3a_ui_content_t words(const char *title, const char *text, int progress)
{
    p3a_ui_content_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.title, sizeof(c.title), "%s", title);
    snprintf(c.text, sizeof(c.text), "%s", text);
    c.progress = (int16_t)progress;
    return c;
}

static ui_view_t view(void)
{
    ui_view_t v;
    ui_model_view(&m, &v);
    return v;
}

static void test_with_nothing_up_the_artwork_is_seen(void)
{
    ui_model_init(&m);
    const ui_view_t v = view();
    TEST_ASSERT_EQUAL(P3A_SCREEN_ARTWORK, v.screen);
    TEST_ASSERT_FALSE(v.banner);
    TEST_ASSERT_FALSE(v.covers_artwork);
    TEST_ASSERT_FALSE(v.holds_playback);
    TEST_ASSERT_NULL(ui_model_content(&m));
    TEST_ASSERT_FALSE(ui_model_tick(&m, 1000));
    TEST_ASSERT_EQUAL(0, m.revision);
}

static void test_the_screen_listed_last_is_seen_and_taking_it_down_uncovers_the_next(void)
{
    ui_model_init(&m);
    p3a_ui_content_t info = words("Info", "p3a-fab", -1), setup = words("Setup", "Join p3a-setup", -1);
    TEST_ASSERT_TRUE(ui_model_screen_show(&m, P3A_SCREEN_INFO, &info));
    TEST_ASSERT_EQUAL(P3A_SCREEN_INFO, view().screen);
    TEST_ASSERT_TRUE(view().covers_artwork);
    TEST_ASSERT_TRUE(view().holds_playback);
    TEST_ASSERT_TRUE(ui_model_screen_show(&m, P3A_SCREEN_SETUP, &setup));
    TEST_ASSERT_EQUAL(P3A_SCREEN_SETUP, view().screen);
    TEST_ASSERT_EQUAL_STRING("Join p3a-setup", ui_model_content(&m)->text);

    // Something lower changes underneath: nothing seen changes.
    p3a_ui_content_t info2 = words("Info", "p3a-fab, 192.168.4.42", -1);
    const uint32_t revision = m.revision;
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_INFO, &info2));
    TEST_ASSERT_FALSE(ui_model_screen_hide(&m, P3A_SCREEN_REGISTRATION));  // was not up
    TEST_ASSERT_EQUAL(revision, m.revision);

    TEST_ASSERT_TRUE(ui_model_screen_hide(&m, P3A_SCREEN_SETUP));
    TEST_ASSERT_EQUAL(P3A_SCREEN_INFO, view().screen);
    TEST_ASSERT_EQUAL_STRING("p3a-fab, 192.168.4.42", ui_model_content(&m)->text);
    TEST_ASSERT_TRUE(ui_model_screen_hide(&m, P3A_SCREEN_INFO));
    TEST_ASSERT_EQUAL(P3A_SCREEN_ARTWORK, view().screen);
}

static void test_the_same_words_again_are_not_a_change_and_new_words_are(void)
{
    ui_model_init(&m);
    p3a_ui_content_t a = words("Updating", "Do not unplug", 10), b = words("Updating", "Do not unplug", 11);
    TEST_ASSERT_TRUE(ui_model_screen_show(&m, P3A_SCREEN_UPDATE, &a));
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_UPDATE, &a));
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_UPDATE, NULL));  // keeps its words
    TEST_ASSERT_TRUE(ui_model_screen_show(&m, P3A_SCREEN_UPDATE, &b));
    TEST_ASSERT_EQUAL(11, ui_model_content(&m)->progress);
    TEST_ASSERT_EQUAL(2, m.revision);
    TEST_ASSERT_EQUAL(1, m.screens[P3A_SCREEN_UPDATE].shows);
}

static void test_the_fatal_screen_is_above_all_and_never_comes_down(void)
{
    ui_model_init(&m);
    p3a_ui_content_t fatal = words("Cannot continue", "The panel memory is damaged", -1);
    ui_model_screen_show(&m, P3A_SCREEN_USB, NULL);
    TEST_ASSERT_TRUE(ui_model_screen_show(&m, P3A_SCREEN_FATAL, &fatal));
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_UPDATE, NULL));  // up, but not seen
    TEST_ASSERT_FALSE(ui_model_screen_hide(&m, P3A_SCREEN_FATAL));
    TEST_ASSERT_EQUAL(P3A_SCREEN_FATAL, view().screen);
}

static void test_the_artwork_and_notice_rows_are_not_screens_to_put_up(void)
{
    ui_model_init(&m);
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_ARTWORK, NULL));
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_NOTICE, NULL));
    TEST_ASSERT_FALSE(ui_model_screen_show(&m, P3A_SCREEN_COUNT, NULL));
    TEST_ASSERT_FALSE(ui_model_screen_hide(&m, P3A_SCREEN_COUNT));
    TEST_ASSERT_FALSE(ui_model_notice_show(&m, P3A_NOTICE_COUNT, NULL, 0, 0, 0));
    TEST_ASSERT_FALSE(ui_model_notice_hide(&m, P3A_NOTICE_COUNT));
    TEST_ASSERT_EQUAL(P3A_SCREEN_ARTWORK, view().screen);
}

static void test_a_notice_is_the_whole_screen_with_nothing_playing_and_a_banner_over_an_artwork(void)
{
    ui_model_init(&m);
    p3a_ui_content_t loading = words("Loading", "Listing your files", -1);
    TEST_ASSERT_TRUE(ui_model_notice_show(&m, P3A_NOTICE_LOADING, &loading, 0, 0, 0));
    ui_view_t v = view();
    TEST_ASSERT_EQUAL(P3A_SCREEN_NOTICE, v.screen);
    TEST_ASSERT_EQUAL(P3A_NOTICE_LOADING, v.notice);
    TEST_ASSERT_TRUE(v.covers_artwork);
    TEST_ASSERT_FALSE(v.banner);
    TEST_ASSERT_FALSE(v.holds_playback);

    TEST_ASSERT_TRUE(ui_model_set_playing(&m, true));
    v = view();
    TEST_ASSERT_EQUAL(P3A_SCREEN_ARTWORK, v.screen);
    TEST_ASSERT_TRUE(v.banner);
    TEST_ASSERT_FALSE(v.covers_artwork);
    TEST_ASSERT_EQUAL_STRING("Listing your files", ui_model_content(&m)->text);
    TEST_ASSERT_FALSE(ui_model_set_playing(&m, true));

    // Under a screen a notice is not seen at all.
    ui_model_screen_show(&m, P3A_SCREEN_INFO, NULL);
    v = view();
    TEST_ASSERT_EQUAL(P3A_SCREEN_INFO, v.screen);
    TEST_ASSERT_FALSE(v.banner);
    TEST_ASSERT_TRUE(ui_model_screen_hide(&m, P3A_SCREEN_INFO));
    TEST_ASSERT_TRUE(view().banner);
    TEST_ASSERT_TRUE(ui_model_notice_hide(&m, P3A_NOTICE_LOADING));
    TEST_ASSERT_FALSE(view().banner);
}

static void test_the_more_important_notice_is_seen_and_the_other_returns_when_it_goes(void)
{
    ui_model_init(&m);
    p3a_ui_content_t loading = words("Loading", "", -1), failed = words("SD card", "The card cannot be used", -1);
    ui_model_notice_show(&m, P3A_NOTICE_LOADING, &loading, 0, 0, 0);
    TEST_ASSERT_TRUE(ui_model_notice_show(&m, P3A_NOTICE_CARD_FAILED, &failed, 5000, 0, 1000));
    TEST_ASSERT_EQUAL(P3A_NOTICE_CARD_FAILED, view().notice);
    // A change to the one underneath is not a change to what is seen.
    p3a_ui_content_t loading2 = words("Loading", "3 channels", -1);
    TEST_ASSERT_FALSE(ui_model_notice_show(&m, P3A_NOTICE_LOADING, &loading2, 0, 0, 1000));
    TEST_ASSERT_FALSE(ui_model_tick(&m, 5999));
    TEST_ASSERT_TRUE(ui_model_tick(&m, 6000));
    TEST_ASSERT_EQUAL(P3A_NOTICE_LOADING, view().notice);
    TEST_ASSERT_EQUAL_STRING("3 channels", ui_model_content(&m)->text);
    TEST_ASSERT_FALSE(ui_model_tick(&m, 999999));  // no ttl: it stays
}

static void test_showing_again_extends_the_time_and_a_cap_limits_shows_per_boot(void)
{
    ui_model_init(&m);
    p3a_ui_content_t busy = words("Giphy", "Too many requests; trying again later", -1);
    TEST_ASSERT_TRUE(ui_model_notice_show(&m, P3A_NOTICE_SOURCE_PROBLEM, &busy, 20000, 2, 0));
    TEST_ASSERT_FALSE(ui_model_notice_show(&m, P3A_NOTICE_SOURCE_PROBLEM, &busy, 20000, 2, 15000));  // same words
    TEST_ASSERT_FALSE(ui_model_tick(&m, 20000));  // extended to 35000
    TEST_ASSERT_TRUE(ui_model_tick(&m, 35000));
    TEST_ASSERT_EQUAL(1, m.notices[P3A_NOTICE_SOURCE_PROBLEM].shows);

    TEST_ASSERT_TRUE(ui_model_notice_show(&m, P3A_NOTICE_SOURCE_PROBLEM, &busy, 20000, 2, 100000));
    ui_model_tick(&m, 120000);
    // The third time in this boot it is not shown.
    TEST_ASSERT_FALSE(ui_model_notice_show(&m, P3A_NOTICE_SOURCE_PROBLEM, &busy, 20000, 2, 200000));
    TEST_ASSERT_EQUAL(P3A_SCREEN_ARTWORK, view().screen);
    // Without a cap it always is.
    TEST_ASSERT_TRUE(ui_model_notice_show(&m, P3A_NOTICE_SOURCE_PROBLEM, &busy, 0, 0, 200000));
}

static void test_words_too_long_are_cut_and_stay_terminated(void)
{
    ui_model_init(&m);
    p3a_ui_content_t c;
    memset(&c, 'x', sizeof(c));  // no terminator anywhere
    c.progress = 50;
    ui_model_screen_show(&m, P3A_SCREEN_UPDATE, &c);
    const p3a_ui_content_t *got = ui_model_content(&m);
    TEST_ASSERT_EQUAL(P3A_UI_TITLE_LEN - 1, strlen(got->title));
    TEST_ASSERT_EQUAL(P3A_UI_TEXT_LEN - 1, strlen(got->text));
}

static void test_every_screen_and_notice_has_a_name(void)
{
    for (int i = 0; i < P3A_SCREEN_COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", ui_screen_name((p3a_screen_t)i)));
    }
    for (int i = 0; i < P3A_NOTICE_COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", ui_notice_name((p3a_notice_t)i)));
    }
    TEST_ASSERT_EQUAL_STRING("unknown", ui_screen_name(P3A_SCREEN_COUNT));
    TEST_ASSERT_EQUAL_STRING("unknown", ui_notice_name(P3A_NOTICE_COUNT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_with_nothing_up_the_artwork_is_seen);
    RUN_TEST(test_the_screen_listed_last_is_seen_and_taking_it_down_uncovers_the_next);
    RUN_TEST(test_the_same_words_again_are_not_a_change_and_new_words_are);
    RUN_TEST(test_the_fatal_screen_is_above_all_and_never_comes_down);
    RUN_TEST(test_the_artwork_and_notice_rows_are_not_screens_to_put_up);
    RUN_TEST(test_a_notice_is_the_whole_screen_with_nothing_playing_and_a_banner_over_an_artwork);
    RUN_TEST(test_the_more_important_notice_is_seen_and_the_other_returns_when_it_goes);
    RUN_TEST(test_showing_again_extends_the_time_and_a_cap_limits_shows_per_boot);
    RUN_TEST(test_words_too_long_are_cut_and_stay_terminated);
    RUN_TEST(test_every_screen_and_notice_has_a_name);
    return UNITY_END();
}
