// SPDX-License-Identifier: Apache-2.0
#include "p3a_event_table.h"

#include <string.h>

void p3a_event_table_init(p3a_event_table_t *table)
{
    memset(table, 0, sizeof(*table));
}

static bool same(const p3a_event_sub_t *sub, uint16_t type, p3a_event_handler_t handler, void *ctx)
{
    return sub->type == type && sub->handler == handler && sub->ctx == ctx;
}

esp_err_t p3a_event_table_add(p3a_event_table_t *table, uint16_t type, p3a_event_handler_t handler, void *ctx)
{
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < table->count; i++) {
        if (same(&table->subs[i], type, handler, ctx)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (table->count >= P3A_EVENT_MAX_SUBSCRIBERS) {
        return ESP_ERR_NO_MEM;
    }
    table->subs[table->count++] = (p3a_event_sub_t){.type = type, .handler = handler, .ctx = ctx};
    return ESP_OK;
}

bool p3a_event_table_remove(p3a_event_table_t *table, uint16_t type, p3a_event_handler_t handler, void *ctx)
{
    for (size_t i = 0; i < table->count; i++) {
        if (same(&table->subs[i], type, handler, ctx)) {
            memmove(&table->subs[i], &table->subs[i + 1], (table->count - i - 1) * sizeof(table->subs[0]));
            table->count--;
            return true;
        }
    }
    return false;
}

size_t p3a_event_table_match(const p3a_event_table_t *table, uint16_t type, p3a_event_sub_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < table->count && n < max; i++) {
        if (table->subs[i].type == type) {
            out[n++] = table->subs[i];
        }
    }
    return n;
}
