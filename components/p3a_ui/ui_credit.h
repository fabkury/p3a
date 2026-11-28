// SPDX-License-Identifier: Apache-2.0
// The credit under an artwork: where a source's mark ("Powered by GIPHY",
// as the service drew it) goes when its artwork leaves a band free below
// it. Pure: host-tested (tests/host/ui/test_ui_credit.c). Private to the UI.
//
// The mark is drawn only in the band below the artwork, never over it: an
// artwork that fills the screen, or leaves a band too low for the mark
// with a margin above and below, has no credit. The mark is drawn at its
// own size, in the middle of the band's height, against the right. When
// the clock is on and set to the bottom right, the mark goes against the
// left instead, so that neither covers the other.
//
// All in the viewer's coordinates, so the screen's rotation plays no part:
// "below" is below as the picture is seen.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CREDIT_MARGIN 4         // clear rows above and below the mark, at least
#define UI_CREDIT_SIDE_MARGIN 12  // clear columns between the mark and the screen's edge beside it

typedef struct {
    bool show;
    int32_t x, y;  // the mark's top left corner
} ui_credit_place_t;

// True when the mark goes against the left: the clock is shown
// (`clock_on`) where the mark would be (`clock_place`, a ui_clock_place_t).
bool ui_credit_at_left(bool clock_on, int clock_place);

// `src_w` by `src_h` is the artwork's own size, `view_w` by `view_h` the
// screen as seen, `mark_w` by `mark_h` the mark; `at_left` puts it against
// the left of the band, not the right.
ui_credit_place_t ui_credit_place(int32_t src_w, int32_t src_h, int32_t view_w, int32_t view_h, int32_t mark_w,
                                  int32_t mark_h, bool at_left);

#ifdef __cplusplus
}
#endif
