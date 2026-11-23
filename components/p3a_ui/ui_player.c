// SPDX-License-Identifier: Apache-2.0
// What the screen should say about playback. See ui_player.h. Pure.

#include "ui_player.h"

#include "p3a_err.h"

ui_player_standing_t ui_player_standing(const ui_player_facts_t *facts, p3a_ui_content_t *out)
{
    if (facts->playing) {
        return UI_PLAYER_NONE;
    }
    if (facts->listing > 0 || facts->loading) {
        p3a_ui_content(out, "Getting ready", -1, "Looking for artworks");
        return UI_PLAYER_LOADING;
    }
    if (facts->playable > 0) {
        return UI_PLAYER_NONE;  // between two turns; the scheduler will ask again
    }
    if (facts->wanted > 0 && facts->online && !facts->card_failed) {
        p3a_ui_content(out, "Getting ready", -1, "Downloading artworks");
        return UI_PLAYER_LOADING;
    }
    if (facts->card_failed) {
        p3a_ui_content(out, "Nothing to play", -1, "The SD card could not be read");
    } else if (facts->channels > 0 && facts->no_source == facts->channels) {
        p3a_ui_content(out, "Nothing to play", -1, "This playset's sources are not part of this firmware");
    } else if (facts->offline > 0 || facts->wanted > 0) {
        p3a_ui_content(out, "Nothing to play yet", -1, "Waiting for the network");
    } else if (facts->need_key) {
        p3a_ui_content(out, "Nothing to play", -1, "A source needs its key. Enter it on the web page");
    } else if (facts->rate_limited) {
        p3a_ui_content(out, "Nothing to play yet", -1, "A source asked to wait. Trying again later");
    } else if (facts->failed > 0) {
        p3a_ui_content(out, "Nothing to play", -1, "%u of %u channels could not be loaded", (unsigned)facts->failed,
                       (unsigned)facts->channels);
    } else {
        p3a_ui_content(out, "Nothing to play", -1, "Add artworks from the web page or copy them to the SD card");
    }
    return UI_PLAYER_NOTHING;
}

void ui_player_problem(int problem, p3a_ui_content_t *out)
{
    switch (problem) {
    case P3A_ERR_MISSING:
        p3a_ui_content(out, "Not shown", -1, "That artwork is no longer on the device");
        break;
    case P3A_ERR_CORRUPT:
        p3a_ui_content(out, "Not shown", -1, "That file is damaged");
        break;
    case P3A_ERR_NOT_PLAYABLE:
        p3a_ui_content(out, "Not shown", -1, "This device cannot play that file");
        break;
    case P3A_ERR_SD_FAILED:
        p3a_ui_content(out, "Not shown", -1, "The SD card could not be read");
        break;
    default:
        p3a_ui_content(out, "Not shown", -1, "The artwork could not be loaded");
        break;
    }
}

void ui_indicator_update(ui_indicator_t *indicator, const ui_player_facts_t *facts, int64_t now_ms)
{
    if (indicator->primed && facts->problems != indicator->problems_seen) {
        indicator->failed_until_ms = now_ms + UI_INDICATOR_FAILED_MS;
    }
    indicator->primed = true;
    indicator->problems_seen = facts->problems;
    indicator->busy = facts->asked;
    if (facts->asked) {
        indicator->failed_until_ms = 0;  // asking again clears the red at once
    }
}

ui_indicator_show_t ui_indicator_now(const ui_indicator_t *indicator, int64_t now_ms)
{
    if (indicator->busy) {
        return UI_INDICATOR_BUSY;
    }
    return now_ms < indicator->failed_until_ms ? UI_INDICATOR_FAILED : UI_INDICATOR_OFF;
}
