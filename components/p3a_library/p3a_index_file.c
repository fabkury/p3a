// SPDX-License-Identifier: Apache-2.0
// An index on the SD card: its records as one framed container.
#include <stdlib.h>
#include <string.h>

#include "p3a_frame.h"
#include "p3a_index.h"
#include "psram_alloc.h"

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

// --- a channel's file, shared by playsets ------------------------------------------

static const p3a_frame_expect_t s_whole_file = {.schema = P3A_FRAME_SCHEMA_INDEX,
                                                .version = P3A_RECORD_VERSION,
                                                .record_size = sizeof(p3a_record_t),
                                                .max_count = P3A_INDEX_MAX_RECORDS};

esp_err_t p3a_index_load_shared(p3a_index_t *index, const char *path, p3a_index_listing_t *listing,
                                uint32_t *in_file)
{
    void *records = NULL;
    uint32_t count = 0;
    memset(listing, 0, sizeof(*listing));
    *in_file = 0;
    esp_err_t err = p3a_frame_load(path, &s_whole_file, &records, &count, NULL);
    if (err != ESP_OK) {
        p3a_index_clear(index);
        return err;
    }
    p3a_index_shared_read(records, count, listing);
    err = p3a_index_set_records(index, records, count < index->capacity ? count : index->capacity);
    free(records);
    if (err == ESP_OK) {
        *in_file = count;
    }
    return err;
}

esp_err_t p3a_index_save_shared(const p3a_index_t *mine, const p3a_index_listing_t *listing, bool keep_file,
                                const char *path, uint32_t *written)
{
    void *old = NULL;
    uint32_t old_count = 0;
    p3a_index_listing_t old_listing = {0};
    if (keep_file && p3a_frame_load(path, &s_whole_file, &old, &old_count, NULL) == ESP_OK) {
        p3a_index_shared_read(old, old_count, &old_listing);
    } else {
        old = NULL;
        old_count = 0;
    }
    // At most 768 KiB of PSRAM for the length of this call, beside the file's own records.
    const uint32_t room = old_count > mine->count ? old_count : mine->count;
    p3a_record_t *out = psram_calloc(room > 0 ? room : 1, sizeof(p3a_record_t));
    if (out == NULL) {
        free(old);
        return ESP_ERR_NO_MEM;
    }
    const uint32_t count = p3a_index_shared_pack(mine, listing, old, old_count, &old_listing, out);
    free(old);
    const esp_err_t err = p3a_frame_save(path, P3A_FRAME_SCHEMA_INDEX, P3A_RECORD_VERSION, sizeof(p3a_record_t), count,
                                         P3A_FRAME_REBUILDABLE, out);
    free(out);
    if (err == ESP_OK && written != NULL) {
        *written = count;
    }
    return err;
}

void p3a_index_carry_over_file(p3a_index_t *fresh, const char *path)
{
    void *records = NULL;
    uint32_t count = 0;
    if (p3a_frame_load(path, &s_whole_file, &records, &count, NULL) != ESP_OK || count == 0) {
        free(records);
        return;
    }
    p3a_index_listing_t listing;
    p3a_index_shared_read(records, count, &listing);
    p3a_index_t known = {0};
    if (p3a_index_create(&known, count) == ESP_OK && p3a_index_set_records(&known, records, count) == ESP_OK) {
        p3a_index_carry_over(fresh, &known, NULL);
    }
    p3a_index_destroy(&known);
    free(records);
}
