// SPDX-License-Identifier: Apache-2.0
// The pure half of p3a_frame.h. Host-tested in tests/host/storage/test_frame.c.
#include "p3a_frame.h"

#include <string.h>

#include "p3a_crc32.h"
#include "p3a_err.h"

static uint32_t header_crc(const p3a_frame_header_t *h)
{
    return p3a_crc32(0, h, offsetof(p3a_frame_header_t, header_crc));
}

void p3a_frame_header_make(p3a_frame_header_t *header, uint16_t schema, uint16_t version, uint16_t record_size,
                           uint32_t count, uint32_t flags, const void *records)
{
    memset(header, 0, sizeof(*header));
    header->magic = P3A_FRAME_MAGIC;
    header->header_size = sizeof(*header);
    header->schema = schema;
    header->version = version;
    header->record_size = record_size;
    header->count = count;
    header->flags = flags;
    header->records_crc = p3a_crc32(0, records, (size_t)count * record_size);
    header->header_crc = header_crc(header);
}

esp_err_t p3a_frame_header_check(const void *data, size_t len, const p3a_frame_expect_t *expect, size_t *records_len)
{
    if (!data || !expect) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < sizeof(p3a_frame_header_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    p3a_frame_header_t h;
    memcpy(&h, data, sizeof(h));  // the buffer may not be aligned

    if (h.magic != P3A_FRAME_MAGIC || h.header_size != sizeof(h) || h.header_crc != header_crc(&h)) {
        return P3A_ERR_CORRUPT;
    }
    // From here on the header is intact: a mismatch is a different firmware's
    // file, not damage.
    if (h.schema != expect->schema || h.version != expect->version || h.record_size != expect->record_size) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (h.count > expect->max_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (records_len) {
        *records_len = (size_t)h.count * h.record_size;
    }
    return ESP_OK;
}

esp_err_t p3a_frame_check(const void *data, size_t len, const p3a_frame_expect_t *expect, const void **records,
                          uint32_t *count)
{
    size_t records_len = 0;
    esp_err_t err = p3a_frame_header_check(data, len, expect, &records_len);
    if (err != ESP_OK) {
        return err;
    }
    if (len - sizeof(p3a_frame_header_t) < records_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    p3a_frame_header_t h;
    memcpy(&h, data, sizeof(h));
    const uint8_t *body = (const uint8_t *)data + sizeof(h);
    if (p3a_crc32(0, body, records_len) != h.records_crc) {
        return P3A_ERR_CORRUPT;
    }
    if (records) {
        *records = body;
    }
    if (count) {
        *count = h.count;
    }
    return ESP_OK;
}
