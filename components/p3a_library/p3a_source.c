// SPDX-License-Identifier: Apache-2.0
#include "p3a_source.h"

#include <string.h>

// Filled at boot, before anything reads it, and never changed afterwards:
// no lock.
static const p3a_source_ops_t *s_sources[P3A_CHANNEL_SOURCE_END_];

esp_err_t p3a_source_register(const p3a_source_ops_t *ops)
{
    if (ops == NULL || ops->name == NULL || ops->refresh == NULL || ops->source == 0 ||
        ops->source >= P3A_CHANNEL_SOURCE_END_) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sources[ops->source] != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sources[ops->source] = ops;
    return ESP_OK;
}

const p3a_source_ops_t *p3a_source_get(uint8_t source)
{
    return source < P3A_CHANNEL_SOURCE_END_ ? s_sources[source] : NULL;
}

void p3a_source_reset_for_test(void)
{
    memset(s_sources, 0, sizeof(s_sources));
}
