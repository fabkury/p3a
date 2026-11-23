// SPDX-License-Identifier: Apache-2.0
#include "p3a_record.h"

#include <string.h>

#include "p3a_err.h"

uint32_t p3a_record_hash(p3a_source_t source, const void *bytes, size_t len)
{
    // FNV-1a, seeded with the source so that the same text at two sources
    // does not give the same key.
    uint32_t h = 2166136261u;
    h = (h ^ (uint8_t)source) * 16777619u;
    const uint8_t *p = bytes;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h != 0 ? h : 1;
}

static void start(p3a_record_t *record, p3a_source_t source)
{
    memset(record, 0, sizeof(*record));
    record->source = (uint8_t)source;
}

// Copies a text identifier into a field of `room` bytes, terminator included.
static esp_err_t put_text(char *field, size_t room, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strlen(text);
    if (len >= room) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(field, text, len);  // the rest of the field is already zero
    return ESP_OK;
}

esp_err_t p3a_record_init_local(p3a_record_t *record, const char *filename)
{
    start(record, P3A_SOURCE_LOCAL);
    esp_err_t err = put_text(record->identity.local.filename, P3A_RECORD_FILENAME_LEN, filename);
    if (err == ESP_OK) {
        record->id32 = p3a_record_hash(P3A_SOURCE_LOCAL, filename, strlen(filename));
    }
    return err;
}

esp_err_t p3a_record_init_makapix(p3a_record_t *record, uint32_t post_id, const uint8_t uuid[16])
{
    start(record, P3A_SOURCE_MAKAPIX);
    if (post_id == 0 || uuid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(record->identity.makapix.uuid, uuid, 16);
    record->id32 = post_id;
    return ESP_OK;
}

esp_err_t p3a_record_init_giphy(p3a_record_t *record, const char *id)
{
    start(record, P3A_SOURCE_GIPHY);
    esp_err_t err = put_text(record->identity.giphy.id, P3A_RECORD_GIPHY_ID_LEN, id);
    if (err == ESP_OK) {
        record->id32 = p3a_record_hash(P3A_SOURCE_GIPHY, id, strlen(id));
    }
    return err;
}

esp_err_t p3a_record_init_klipy(p3a_record_t *record, uint64_t id, uint8_t product)
{
    start(record, P3A_SOURCE_KLIPY);
    if (id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    record->identity.klipy.id = id;
    record->identity.klipy.product = product;
    // Over the identity as stored: the nine bytes of id and product.
    record->id32 = p3a_record_hash(P3A_SOURCE_KLIPY, &record->identity.klipy, sizeof(record->identity.klipy));
    return ESP_OK;
}

esp_err_t p3a_record_init_museum(p3a_record_t *record, uint16_t museum_id, const char *iiif_key)
{
    start(record, P3A_SOURCE_MUSEUM);
    esp_err_t err = put_text(record->identity.museum.iiif_key, P3A_RECORD_IIIF_KEY_LEN, iiif_key);
    if (err == ESP_OK) {
        record->identity.museum.museum_id = museum_id;
        // Over the museum's number and the key: two museums may use the same key.
        record->id32 = p3a_record_hash(P3A_SOURCE_MUSEUM, &record->identity.museum, 2 + strlen(iiif_key));
    }
    return err;
}

// How many bytes of the identity a source uses; the rest must be zero.
static size_t identity_used(const p3a_record_t *record)
{
    switch (record->source) {
        case P3A_SOURCE_LOCAL:   return sizeof(record->identity.local);
        case P3A_SOURCE_MAKAPIX: return sizeof(record->identity.makapix);
        case P3A_SOURCE_GIPHY:   return sizeof(record->identity.giphy);
        case P3A_SOURCE_KLIPY:   return sizeof(record->identity.klipy);
        case P3A_SOURCE_MUSEUM:  return sizeof(record->identity.museum);
        default:                 return 0;
    }
}

static bool all_zero(const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

// A text field holds a non-empty string, terminated inside the field, with
// nothing but zeros after the terminator.
static bool text_ok(const char *field, size_t room)
{
    const char *end = memchr(field, '\0', room);
    if (end == NULL || end == field) {
        return false;
    }
    return all_zero((const uint8_t *)end, room - (size_t)(end - field));
}

esp_err_t p3a_record_check(const p3a_record_t *record)
{
    if (record->source == P3A_SOURCE_NONE || record->source >= P3A_SOURCE_END_ || record->kind != 0 ||
        record->format >= P3A_FORMAT_END_ || record->id32 == 0 ||
        (record->flags & ~P3A_RECORD_FLAGS_KNOWN) != 0 ||
        !all_zero(record->reserved, sizeof(record->reserved))) {
        return P3A_ERR_CORRUPT;
    }
    const size_t used = identity_used(record);
    if (!all_zero(record->identity.raw + used, sizeof(record->identity.raw) - used)) {
        return P3A_ERR_CORRUPT;
    }
    bool ok = true;
    switch (record->source) {
        case P3A_SOURCE_LOCAL:
            ok = text_ok(record->identity.local.filename, P3A_RECORD_FILENAME_LEN);
            break;
        case P3A_SOURCE_GIPHY:
            ok = text_ok(record->identity.giphy.id, P3A_RECORD_GIPHY_ID_LEN);
            break;
        case P3A_SOURCE_MUSEUM:
            ok = text_ok(record->identity.museum.iiif_key, P3A_RECORD_IIIF_KEY_LEN);
            break;
        case P3A_SOURCE_KLIPY:
            ok = record->identity.klipy.id != 0;
            break;
        default:
            break;
    }
    return ok ? ESP_OK : P3A_ERR_CORRUPT;
}

bool p3a_record_same_identity(const p3a_record_t *a, const p3a_record_t *b)
{
    // Unused identity bytes are zero in every record that was made by an init
    // function or passed p3a_record_check(), so the whole field compares.
    return a->source == b->source && memcmp(a->identity.raw, b->identity.raw, sizeof(a->identity.raw)) == 0;
}

bool p3a_record_has_blob(const p3a_record_t *record)
{
    return !all_zero(record->blob_ref, sizeof(record->blob_ref));
}

const char *p3a_source_name(uint8_t source)
{
    switch (source) {
        case P3A_SOURCE_LOCAL:   return "local";
        case P3A_SOURCE_MAKAPIX: return "makapix";
        case P3A_SOURCE_GIPHY:   return "giphy";
        case P3A_SOURCE_KLIPY:   return "klipy";
        case P3A_SOURCE_MUSEUM:  return "museum";
        default:                 return "unknown";
    }
}
