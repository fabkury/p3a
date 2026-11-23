// SPDX-License-Identifier: Apache-2.0
// The notices about playback, from the player's state.
#include <string.h>

#include "p3a_err.h"
#include "ui_player.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static p3a_ui_content_t c;

static void test_while_something_plays_there_is_no_standing_notice(void)
{
    ui_player_facts_t f = {.playing = true, .channels = 2, .listing = 1, .playable = 0};
    TEST_ASSERT_EQUAL(UI_PLAYER_NONE, ui_player_standing(&f, &c));
    f.loading = true;
    TEST_ASSERT_EQUAL(UI_PLAYER_NONE, ui_player_standing(&f, &c));
}

static void test_loading_until_every_channel_is_listed_and_the_first_artwork_is_up(void)
{
    ui_player_facts_t f = {.channels = 3, .listing = 3};
    TEST_ASSERT_EQUAL(UI_PLAYER_LOADING, ui_player_standing(&f, &c));
    TEST_ASSERT_EQUAL_STRING("Getting ready", c.title);
    TEST_ASSERT_EQUAL(-1, c.progress);
    // Listed, one found, and it is being loaded.
    f = (ui_player_facts_t){.channels = 3, .listing = 0, .playable = 4, .loading = true};
    TEST_ASSERT_EQUAL(UI_PLAYER_LOADING, ui_player_standing(&f, &c));
    // Between a failed turn and the next attempt: no notice flashes up.
    f.loading = false;
    TEST_ASSERT_EQUAL(UI_PLAYER_NONE, ui_player_standing(&f, &c));
}

static void test_nothing_to_play_says_why(void)
{
    ui_player_facts_t f = {.channels = 1};
    TEST_ASSERT_EQUAL(UI_PLAYER_NOTHING, ui_player_standing(&f, &c));
    TEST_ASSERT_EQUAL_STRING("Nothing to play", c.title);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "Add artworks"));

    f = (ui_player_facts_t){.channels = 2, .no_source = 2};
    ui_player_standing(&f, &c);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "not part of this firmware"));

    f = (ui_player_facts_t){.channels = 3, .no_source = 1, .failed = 2};
    ui_player_standing(&f, &c);
    TEST_ASSERT_EQUAL_STRING("2 of 3 channels could not be loaded", c.text);

    f.card_failed = true;
    ui_player_standing(&f, &c);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "SD card"));
}

static void test_a_failed_request_is_put_in_plain_words(void)
{
    ui_player_problem(P3A_ERR_MISSING, &c);
    TEST_ASSERT_EQUAL_STRING("Not shown", c.title);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "no longer"));
    ui_player_problem(P3A_ERR_CORRUPT, &c);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "damaged"));
    ui_player_problem(P3A_ERR_NOT_PLAYABLE, &c);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "cannot play"));
    ui_player_problem(ESP_ERR_NO_MEM, &c);
    TEST_ASSERT_NOT_NULL(strstr(c.text, "could not be loaded"));
    TEST_ASSERT_NULL(strstr(c.text, "ESP_"));  // never an error code on the screen
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_while_something_plays_there_is_no_standing_notice);
    RUN_TEST(test_loading_until_every_channel_is_listed_and_the_first_artwork_is_up);
    RUN_TEST(test_nothing_to_play_says_why);
    RUN_TEST(test_a_failed_request_is_put_in_plain_words);
    return UNITY_END();
}
