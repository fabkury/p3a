// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Giphy channel entry (64 bytes, same size as makapix_channel_entry_t)
 *
 * Stored in channel_cache_t alongside Ci/LAi infrastructure.
 * The post_id is a salted DJB2 hash of giphy_id (negative).
 */
typedef struct __attribute__((packed)) {
    int32_t post_id;              ///< DJB2 hash of giphy_id (negative, salt=0x47495048)
    uint8_t kind;                 ///< 0 = gif artwork
    uint8_t extension;            ///< 0=webp, 1=gif
    uint16_t width;               ///< Rendition width in pixels
    uint32_t created_at;          ///< Unix timestamp (from trending_datetime or import_datetime)
    uint16_t height;              ///< Rendition height in pixels
    char giphy_id[24];            ///< Giphy string ID (null-terminated, max 23 chars)
    uint8_t reserved[26];         ///< Future use (keeps struct 64 bytes)
} giphy_channel_entry_t;

_Static_assert(sizeof(giphy_channel_entry_t) == 64, "giphy entry must be 64 bytes");

/**
 * @brief DJB2 salt for Giphy post_id generation
 *
 * Different from the standard DJB2 seed (5381) used by SD card channel,
 * to avoid post_id collisions when mixing Giphy and SD card in the same playset.
 * 0x47495048 = "GIPH" in ASCII.
 */
#define GIPHY_DJB2_SALT 0x47495048u

#ifdef __cplusplus
}
#endif
