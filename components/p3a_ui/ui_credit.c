// SPDX-License-Identifier: Apache-2.0
// Where a source's credit goes. See ui_credit.h. Pure.

#include "ui_credit.h"

#include "ui_clock.h"

bool ui_credit_at_left(bool clock_on, int clock_place)
{
    return clock_on && clock_place == UI_CLOCK_BOTTOM_RIGHT;
}

ui_credit_place_t ui_credit_place(int32_t src_w, int32_t src_h, int32_t view_w, int32_t view_h, int32_t mark_w,
                                  int32_t mark_h, bool at_left)
{
    ui_credit_place_t place = {0};
    if (src_w <= 0 || src_h <= 0 || view_w <= 0 || view_h <= 0 || mark_w <= 0 || mark_h <= 0) {
        return place;
    }
    // The picture as the player fits it (player_fit_compute): as large as it
    // goes without cropping, centred.
    int64_t shown_h = view_h;
    if ((int64_t)src_w * view_h >= (int64_t)src_h * view_w) {
        shown_h = (int64_t)view_w * src_h / src_w;  // wider than the screen: bands above and below
    }
    if (shown_h < 1) {
        shown_h = 1;
    }
    const int32_t top = (int32_t)((view_h - shown_h) / 2);
    const int32_t band_top = top + (int32_t)shown_h;
    const int32_t band = view_h - band_top;
    if ((int64_t)mark_h + 2 * UI_CREDIT_MARGIN <= band && (int64_t)mark_w + 2 * UI_CREDIT_SIDE_MARGIN <= view_w) {
        place.show = true;
        place.x = at_left ? UI_CREDIT_SIDE_MARGIN : view_w - UI_CREDIT_SIDE_MARGIN - mark_w;
        place.y = band_top + (band - mark_h) / 2;
    }
    return place;
}
