// SPDX-License-Identifier: Apache-2.0
// The Giphy source: channels of GIFs from giphy.com.
//
//   kind "trending"                  what is trending now
//   kind "search", identifier "cats" the results of a search
//
// A channel's offset starts the listing that many places down, so that two
// channels of one search can show different slices of it.
//
// It needs the user's own API key (the setting giphy_api_key); without one
// its channels report P3A_ERR_NO_KEY. The key is never compiled in.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot, before the scheduler starts.
esp_err_t source_giphy_start(void);

// Forgets the id Giphy gave this device for its listings (it shapes what
// Giphy serves); a new one is asked for at the next listing.
esp_err_t source_giphy_reset_random_id(void);

#ifdef __cplusplus
}
#endif
