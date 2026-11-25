// SPDX-License-Identifier: Apache-2.0
// A pin list as a channel.
//
//   source "pinned", identifier the list's slug (empty: the active list)
//
// The channel holds the list's records in the order they were pinned. Each
// record keeps its own source, so it is drawn, credited and reacted to as
// that source's artworks are. `offset` leaves out that many of the oldest
// pins (an offset past the end leaves out none).
//
// Nothing is listed over the network and nothing is downloaded: a pin's
// bytes are in the blob store and stay there (p3a_pins.h). The scheduler
// lists the channel again whenever the lists change.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot, after p3a_pins_start().
esp_err_t source_pinned_start(void);

#ifdef __cplusplus
}
#endif
