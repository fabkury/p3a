// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy_types.h
 * @brief Klipy channel entry type and shared constants.
 */

#pragma once

#include <stdint.h>
#include <assert.h>

/// Salt for Klipy post_id derivation ("KLIP" in ASCII).
#define KLIPY_DJB2_SALT 0x4B4C4950u

/// Klipy product: which API endpoint prefix + vault subdir an entry belongs to.
#define KLIPY_PRODUCT_GIF     0
#define KLIPY_PRODUCT_STICKER 1

/**
 * @brief Klipy channel cache entry (64 bytes)
 *
 * Sized to match makapix_channel_entry_t / giphy_channel_entry_t (64 bytes) so
 * it can share the channel_cache_t merge / LAi infrastructure via a reinterpret
 * cast, exactly like the Giphy entry.
 *
 * Unlike Giphy (which stores a short string id and reconstructs the CDN URL at
 * download time), Klipy CDN URLs are opaque per-format random tokens that
 * cannot be reconstructed. We therefore store the stable numeric id and
 * re-resolve the download URL via GET {product}/{id} at download time (see
 * klipy_download.c). Klipy items carry no timestamp, so created_at is stamped
 * with the fetch time.
 */
typedef struct __attribute__((packed)) {
    int32_t  post_id;      ///< @0  DJB2-folded from klipy_id (never 0)
    uint8_t  kind;         ///< @4  0 = animation artwork
    uint8_t  extension;    ///< @5  0=webp, 1=gif
    uint16_t width;        ///< @6  chosen-rendition width (px)
    uint32_t created_at;   ///< @8  fetch time (Klipy has no per-item timestamp)
    uint16_t height;       ///< @12 chosen-rendition height (px)
    uint64_t klipy_id;     ///< @14 stable numeric Klipy id (re-resolve key)
    uint8_t  product;      ///< @22 KLIPY_PRODUCT_GIF / KLIPY_PRODUCT_STICKER
    uint8_t  reserved[41]; ///< @23 future use
} klipy_channel_entry_t;

_Static_assert(sizeof(klipy_channel_entry_t) == 64, "klipy entry must be 64 bytes");
