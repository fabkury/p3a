// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "playback_queue.h"
#include "play_scheduler.h"
#include "config_store.h"
#include <string.h>

static esp_err_t map_artwork_to_request(const ps_artwork_t *artwork, queued_item_t *out)
{
    if (!artwork || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if (artwork->filepath[0] == '\0') {
        out->is_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(out->request.filepath, artwork->filepath, sizeof(out->request.filepath));
    out->request.type = artwork->type;
    out->request.post_id = artwork->post_id;
    out->request.is_live_mode = false;
    out->request.start_time_ms = 0;
    out->request.start_frame = 0;

    if (artwork->dwell_time_ms > 0) {
        out->request.dwell_time_ms = artwork->dwell_time_ms;
    } else {
        out->request.dwell_time_ms = config_store_get_dwell_time();
    }

    out->is_ready = true;
    return ESP_OK;
}

esp_err_t playback_queue_current(queued_item_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_artwork_t artwork = {0};
    esp_err_t err = play_scheduler_current(&artwork);
    if (err != ESP_OK) {
        return err;
    }

    return map_artwork_to_request(&artwork, out);
}

esp_err_t playback_queue_next(queued_item_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_artwork_t artwork = {0};
    esp_err_t err = play_scheduler_next(&artwork);
    if (err != ESP_OK) {
        return err;
    }

    return map_artwork_to_request(&artwork, out);
}

esp_err_t playback_queue_prev(queued_item_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_artwork_t artwork = {0};
    esp_err_t err = play_scheduler_prev(&artwork);
    if (err != ESP_OK) {
        return err;
    }

    return map_artwork_to_request(&artwork, out);
}

esp_err_t playback_queue_peek(queued_item_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_artwork_t artwork = {0};
    esp_err_t err = play_scheduler_peek_next(&artwork);
    if (err != ESP_OK) {
        return err;
    }

    return map_artwork_to_request(&artwork, out);
}
