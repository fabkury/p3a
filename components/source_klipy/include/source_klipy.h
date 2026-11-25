// SPDX-License-Identifier: Apache-2.0
// The Klipy source: channels of GIFs and stickers from klipy.com.
//
//   kind "gif:trending", "sticker:trending"            what is trending now
//   kind "gif:search", identifier "cats"               the results of a search
//   kind "gif:category", identifier "happy birthday"   a category, by its name
//
// (and the same for stickers). A channel's offset starts the listing that
// many places down.
//
// It needs the user's own API key (the setting klipy_api_key); without one
// its channels report P3A_ERR_NO_KEY. The key is never compiled in.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot, before the scheduler starts.
esp_err_t source_klipy_start(void);

#ifdef __cplusplus
}
#endif
