// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sdcard_channel.h
 * @brief Canonical asset_type_t definition shared across the codebase
 */

#ifndef SDCARD_CHANNEL_H
#define SDCARD_CHANNEL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Asset type enumeration for supported animation formats
 * This is the canonical definition - other files should include this header
 */
typedef enum {
    ASSET_TYPE_WEBP,
    ASSET_TYPE_GIF,
    ASSET_TYPE_PNG,
    ASSET_TYPE_JPEG,
    ASSET_TYPE_BMP,
} asset_type_t;

#ifdef __cplusplus
}
#endif

#endif // SDCARD_CHANNEL_H
