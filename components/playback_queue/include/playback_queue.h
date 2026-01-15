// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "animation_swap_request.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    swap_request_t request;
    bool is_ready;
} queued_item_t;

esp_err_t playback_queue_current(queued_item_t *out);
esp_err_t playback_queue_next(queued_item_t *out);
esp_err_t playback_queue_prev(queued_item_t *out);
esp_err_t playback_queue_peek(queued_item_t *out);

#ifdef __cplusplus
}
#endif
