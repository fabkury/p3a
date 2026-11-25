// SPDX-License-Identifier: Apache-2.0
// The credit under an artwork: where a source's line ("Powered by GIPHY")
// goes when its artwork leaves a band free below it. Pure: host-tested
// (tests/host/ui/test_ui_credit.c). Private to the UI.
//
// The line is drawn only in the band below the artwork, never over it: an
// artwork that fills the screen, or leaves a band too low for the text at
// twice its size with a margin, has no credit. The size is the largest of
// 3 and 2 that fits the band and four fifths of the screen's width.
//
// All in the viewer's coordinates, so the screen's rotation plays no part:
// "below" is below as the picture is seen.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CREDIT_MARGIN 4  // clear pixels above and below the text, at least

typedef struct {
    bool show;
    int32_t x, y;   // the text's top left corner
    int32_t scale;  // 2 or 3
} ui_credit_place_t;

// `src_w` by `src_h` is the artwork's own size, `view_w` by `view_h` the
// screen as seen; `text_w` and `text_h` the line's size at scale 1.
ui_credit_place_t ui_credit_place(int32_t src_w, int32_t src_h, int32_t view_w, int32_t view_h, int32_t text_w,
                                  int32_t text_h);

#ifdef __cplusplus
}
#endif
