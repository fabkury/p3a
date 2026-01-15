// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "animation_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    animation_decoder_t *decoder;
    animation_decoder_info_t info;
    uint8_t *file_data;
    size_t file_size;
} loaded_animation_t;

esp_err_t loader_service_load(const char *filepath,
                              animation_decoder_type_t decoder_type,
                              loaded_animation_t *out);
void loader_service_unload(loaded_animation_t *loaded);

#ifdef __cplusplus
}
#endif
