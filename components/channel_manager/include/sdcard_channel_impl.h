// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef SDCARD_CHANNEL_IMPL_H
#define SDCARD_CHANNEL_IMPL_H

#include "channel_interface.h"
#include "sdcard_channel.h"

// NOTE: play_navigator was removed as part of Play Scheduler migration.
// Navigation is now handled by Play Scheduler directly.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SD Card Channel - implements channel_interface
 * 
 * This channel reads animations directly from the SD card's animations directory.
 * Items are the animation files themselves (GIF, WebP, PNG, JPEG).
 */

/**
 * @brief Create a new SD card channel
 * 
 * @param name Channel display name
 * @param animations_dir Directory path to scan (NULL for default)
 * @return Channel handle or NULL on failure
 */
channel_handle_t sdcard_channel_create(const char *name, const char *animations_dir);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_CHANNEL_IMPL_H

