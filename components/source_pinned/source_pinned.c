// SPDX-License-Identifier: Apache-2.0
// A pin list as a channel. See source_pinned.h.

#include "source_pinned.h"

#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "p3a_pins.h"
#include "p3a_source.h"
#include "psram_alloc.h"

static esp_err_t pinned_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    const char *slug = (channel->identifier != NULL && channel->identifier[0] != '\0') ? channel->identifier : NULL;
    if (slug != NULL && !p3a_pin_slug_ok(slug)) {
        return P3A_ERR_MISSING;
    }
    esp_err_t err = p3a_pins_load(slug, index);
    if (err == ESP_OK && channel->offset > 0 && channel->offset < index->count) {
        // The list without its first `offset` pins. Through a copy: an index
        // is not filled from its own records.
        const uint32_t rest = index->count - channel->offset;
        p3a_record_t *copy = psram_malloc((size_t)rest * sizeof(p3a_record_t));
        if (copy == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, index->records + channel->offset, (size_t)rest * sizeof(p3a_record_t));
        err = p3a_index_set_records(index, copy, rest);
        free(copy);
    }
    return err;
}

static uint32_t pinned_index_capacity(void)
{
    return P3A_PIN_LIST_MAX;
}

esp_err_t source_pinned_start(void)
{
    static const p3a_source_ops_t ops = {
        .source = P3A_CHANNEL_SOURCE_PINNED,
        .name = "pinned",
        .refresh = pinned_refresh,
        .index_capacity = pinned_index_capacity,
        // `smooth`, `path`, `credit` and `react` are asked of each record's own source.
    };
    return p3a_source_register(&ops);
}
