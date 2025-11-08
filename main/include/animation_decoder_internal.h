/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ANIMATION_DECODER_INTERNAL_H
#define ANIMATION_DECODER_INTERNAL_H

#include "animation_decoder.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Internal structure definition - shared between decoders
// Note: WebP-specific types are forward declared as void* to avoid dependencies
struct animation_decoder_s {
    animation_decoder_type_t type;
    union {
        struct {
            void *decoder; // WebPAnimDecoder* (opaque)
            void *info;    // WebPAnimInfo* (opaque)
            const uint8_t *data;
            size_t data_size;
            bool initialized;
        } webp;
        struct {
            void *gif_decoder; // Opaque GIF decoder pointer
        } gif;
    } impl;
};

#endif // ANIMATION_DECODER_INTERNAL_H

