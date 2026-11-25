// SPDX-License-Identifier: Apache-2.0
// The museum source: channels of artworks from public museum collections.
//
//   kind "{museum}:{axis}", identifier "{term}"
//
// for example kind "vam:category", identifier "THES48903" (the Victoria
// and Albert Museum's prints). The museums and their axes are in the
// component's museum_*.c files. A channel's offset starts the listing that
// many artworks down.
//
// Pictures are fetched as JPEG, at most 720 pixels on the longer side.
// Harvard and the Smithsonian need the user's own API key (settings);
// without it their channels report P3A_ERR_NO_KEY.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the source. Call once at boot, before the scheduler starts.
esp_err_t source_museum_start(void);

#ifdef __cplusplus
}
#endif
