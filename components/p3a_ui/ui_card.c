// SPDX-License-Identifier: Apache-2.0
// Formatting the SD card on the device. See ui_card.h. Pure.

#include "ui_card.h"

#include <stdio.h>
#include <string.h>

void ui_card_init(ui_card_t *card)
{
    memset(card, 0, sizeof(*card));
}

void ui_card_offer(ui_card_t *card)
{
    if (card->phase == UI_CARD_NONE) {
        card->phase = UI_CARD_OFFER;
        card->from_boot = true;
    }
}

static void warn(ui_card_t *card, int64_t now_ms)
{
    card->phase = UI_CARD_WARNING;
    card->deadline_ms = now_ms + UI_CARD_CONFIRM_AFTER_MS;
}

void ui_card_ask(ui_card_t *card, int64_t now_ms)
{
    if (card->phase == UI_CARD_NONE) {
        card->from_boot = false;
        warn(card, now_ms);
    }
}

static void cancel(ui_card_t *card)
{
    card->phase = card->from_boot ? UI_CARD_OFFER : UI_CARD_NONE;
}

ui_card_command_t ui_card_button(ui_card_t *card, int button, int64_t now_ms)
{
    switch (card->phase) {
    case UI_CARD_OFFER:
        if (button == 0) {
            card->phase = UI_CARD_CHECKING;
            return UI_CARD_DO_PROBE;
        }
        break;
    case UI_CARD_WARNING:
        if (button == 0 && now_ms >= card->deadline_ms) {
            card->phase = UI_CARD_FORMATTING;
            return UI_CARD_DO_FORMAT;
        }
        if (button == 1) {
            cancel(card);
        }
        break;
    default:
        break;  // no buttons in the other phases
    }
    return UI_CARD_DO_NOTHING;
}

void ui_card_long_press(ui_card_t *card)
{
    if (card->phase == UI_CARD_WARNING) {
        cancel(card);
    }
}

ui_card_command_t ui_card_probed(ui_card_t *card, ui_card_found_t found, int64_t now_ms)
{
    if (card->phase != UI_CARD_CHECKING) {
        return UI_CARD_DO_NOTHING;
    }
    switch (found) {
    case UI_CARD_FOUND_GOOD:
        card->phase = UI_CARD_GOOD;
        card->deadline_ms = now_ms + UI_CARD_GOOD_MS;
        break;
    case UI_CARD_FOUND_UNUSABLE:
        warn(card, now_ms);
        break;
    default:
        card->phase = UI_CARD_NO_CARD;
        card->deadline_ms = now_ms + UI_CARD_NO_CARD_MS;
        break;
    }
    return UI_CARD_DO_NOTHING;
}

void ui_card_formatted(ui_card_t *card, bool ok, int64_t now_ms)
{
    if (card->phase != UI_CARD_FORMATTING) {
        return;
    }
    card->phase = ok ? UI_CARD_DONE : UI_CARD_FAILED;
    card->deadline_ms = now_ms + (ok ? UI_CARD_DONE_MS : UI_CARD_FAILED_MS);
}

ui_card_command_t ui_card_tick(ui_card_t *card, int64_t now_ms)
{
    if (now_ms < card->deadline_ms) {
        return UI_CARD_DO_NOTHING;
    }
    switch (card->phase) {
    case UI_CARD_NO_CARD:
        card->phase = UI_CARD_OFFER;
        return UI_CARD_DO_NOTHING;
    case UI_CARD_GOOD:
    case UI_CARD_DONE:
    case UI_CARD_FAILED:
        // Asked for once: the phase stays, the deadline moves out of reach.
        card->deadline_ms = INT64_MAX;
        return UI_CARD_DO_RESTART;
    default:
        return UI_CARD_DO_NOTHING;
    }
}

static void button(p3a_ui_content_t *out, int index, const char *label, bool enabled)
{
    snprintf(out->buttons[index], sizeof(out->buttons[index]), "%s", label);
    if (enabled) {
        out->buttons_enabled |= (uint8_t)(1u << index);
    }
}

bool ui_card_content(const ui_card_t *card, int64_t now_ms, p3a_ui_content_t *out)
{
    switch (card->phase) {
    case UI_CARD_OFFER:
        p3a_ui_content(out, "No usable SD card", -1,
                       "p3a needs a working microSD card.\n\n"
                       "Card inserted? p3a can erase and format it.\n\n"
                       "No card? Power off, unscrew the back plate and insert one.");
        button(out, 0, "Format this card", true);
        return true;
    case UI_CARD_CHECKING:
        p3a_ui_content(out, "SD card", -1, "Looking at the card");
        return true;
    case UI_CARD_NO_CARD:
        p3a_ui_content(out, "No card found", -1, "Nothing answers in the card slot. Power off and insert a microSD card.");
        return true;
    case UI_CARD_GOOD:
        p3a_ui_content(out, "The card works", -1, "It needs no formatting. Restarting to use it");
        return true;
    case UI_CARD_WARNING: {
        const int64_t left_ms = card->deadline_ms - now_ms;
        p3a_ui_content(out, "Erase the SD card?", -1, "Everything on the card will be deleted. This cannot be undone.");
        if (left_ms > 0) {
            char label[P3A_UI_BUTTON_LEN];
            snprintf(label, sizeof(label), "Wait %d", (int)((left_ms + 999) / 1000));
            button(out, 0, label, false);
        } else {
            button(out, 0, "Erase and format", true);
        }
        button(out, 1, "Cancel", true);
        return true;
    }
    case UI_CARD_FORMATTING:
        p3a_ui_content(out, "Formatting", -1, "Do not unplug. This can take a minute");
        return true;
    case UI_CARD_DONE:
        p3a_ui_content(out, "Done", -1, "The card is ready. Restarting");
        return true;
    case UI_CARD_FAILED:
        p3a_ui_content(out, "Format failed", -1, "The card may be defective. Restarting; replace the card if this returns");
        return true;
    default:
        return false;
    }
}
