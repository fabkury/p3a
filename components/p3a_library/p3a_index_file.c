// SPDX-License-Identifier: Apache-2.0
// An index on the SD card: its records as one framed container.
#include <stdlib.h>

#include "p3a_frame.h"
#include "p3a_index.h"

esp_err_t p3a_index_save(const p3a_index_t *index, const char *path, uint32_t flags)
{
    return p3a_frame_save(path, P3A_FRAME_SCHEMA_INDEX, P3A_RECORD_VERSION, sizeof(p3a_record_t), index->count, flags,
                          index->records);
}

esp_err_t p3a_index_load(p3a_index_t *index, const char *path)
{
    const p3a_frame_expect_t expect = {.schema = P3A_FRAME_SCHEMA_INDEX,
                                       .version = P3A_RECORD_VERSION,
                                       .record_size = sizeof(p3a_record_t),
                                       .max_count = index->capacity};
    void *records = NULL;
    uint32_t count = 0;
    // The container is read into a buffer of its own and copied: at most
    // 768 KiB of PSRAM for the length of this call.
    esp_err_t err = p3a_frame_load(path, &expect, &records, &count, NULL);
    if (err != ESP_OK) {
        p3a_index_clear(index);
        return err;
    }
    err = p3a_index_set_records(index, records, count);
    free(records);
    return err;
}
