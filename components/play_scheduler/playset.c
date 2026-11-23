// SPDX-License-Identifier: Apache-2.0
#include "playset.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "p3a_err.h"

typedef struct __attribute__((packed)) {
    char name[PLAYSET_NAME_LEN];
    uint8_t reserved[PLAYSET_RECORD_SIZE - PLAYSET_NAME_LEN];
} playset_header_record_t;

_Static_assert(sizeof(playset_header_record_t) == PLAYSET_RECORD_SIZE, "the header record is one record");

static const char *const s_source_names[PLAYSET_SOURCE_END_] = {
    [PLAYSET_SOURCE_LOCAL] = "local",   [PLAYSET_SOURCE_MAKAPIX] = "makapix", [PLAYSET_SOURCE_GIPHY] = "giphy",
    [PLAYSET_SOURCE_KLIPY] = "klipy",   [PLAYSET_SOURCE_MUSEUM] = "museum",   [PLAYSET_SOURCE_PINNED] = "pinned",
    [PLAYSET_SOURCE_TEST] = "test",
};

const char *playset_source_name(uint8_t source)
{
    return source < PLAYSET_SOURCE_END_ ? s_source_names[source] : NULL;
}

uint8_t playset_source_parse(const char *name)
{
    for (uint8_t i = 1; name != NULL && i < PLAYSET_SOURCE_END_; i++) {
        if (strcmp(name, s_source_names[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static esp_err_t fail(esp_err_t err, char *why, size_t why_len, const char *format, ...)
{
    if (why != NULL && why_len > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(why, why_len, format, args);
        va_end(args);
    }
    return err;
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

// Terminated inside the field, nothing but zeros after the terminator.
static bool text_field_ok(const char *field, size_t room)
{
    const char *end = memchr(field, '\0', room);
    return end != NULL && all_zero((const uint8_t *)end, room - (size_t)(end - field));
}

// A name is shown on screen and used in URLs: no control characters, no slash.
static bool name_chars_ok(const char *name)
{
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

esp_err_t playset_check(const playset_t *playset, char *why, size_t why_len)
{
    if (!text_field_ok(playset->name, sizeof(playset->name))) {
        return fail(P3A_ERR_CORRUPT, why, why_len, "the name is not terminated");
    }
    if (!name_chars_ok(playset->name)) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "the name has a slash or a control character");
    }
    if (playset->count < 1 || playset->count > PLAYSET_MAX_CHANNELS) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "a playset has 1 to %d channels", PLAYSET_MAX_CHANNELS);
    }
    for (uint32_t i = 0; i < playset->count; i++) {
        const playset_channel_t *ch = &playset->channels[i];
        if (!text_field_ok(ch->kind, sizeof(ch->kind)) || !text_field_ok(ch->identifier, sizeof(ch->identifier)) ||
            !text_field_ok(ch->display_name, sizeof(ch->display_name)) ||
            !all_zero(ch->reserved0, sizeof(ch->reserved0)) || !all_zero(ch->reserved, sizeof(ch->reserved))) {
            return fail(P3A_ERR_CORRUPT, why, why_len, "channel %u is damaged", (unsigned)i);
        }
        if (playset_source_name(ch->source) == NULL) {
            return fail(ESP_ERR_INVALID_ARG, why, why_len, "channel %u has an unknown source", (unsigned)i);
        }
        if (ch->weight > PLAYSET_MAX_WEIGHT) {
            return fail(ESP_ERR_INVALID_ARG, why, why_len, "channel %u: weight is at most %u", (unsigned)i,
                        (unsigned)PLAYSET_MAX_WEIGHT);
        }
    }
    return ESP_OK;
}

// --- JSON -----------------------------------------------------------------------

// Copies a JSON string into a field, or refuses.
static esp_err_t take_text(const cJSON *item, const char *key, char *field, size_t room, char *why, size_t why_len)
{
    if (item == NULL) {
        return ESP_OK;  // absent: the field stays empty
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "\"%s\" must be text", key);
    }
    const size_t len = strlen(item->valuestring);
    if (len >= room) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "\"%s\" is %u characters; at most %u fit", key, (unsigned)len,
                    (unsigned)(room - 1));
    }
    memcpy(field, item->valuestring, len);
    return ESP_OK;
}

static esp_err_t take_number(const cJSON *item, const char *key, uint32_t max, uint32_t *out, char *why,
                             size_t why_len)
{
    if (item == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > (double)max ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "\"%s\" must be a whole number from 0 to %u", key,
                    (unsigned)max);
    }
    *out = (uint32_t)item->valuedouble;
    return ESP_OK;
}

static bool known_key(const char *key, const char *const *known, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(key, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t refuse_unknown_keys(const cJSON *object, const char *const *known, size_t count, char *why,
                                     size_t why_len)
{
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL || !known_key(item->string, known, count)) {
            return fail(ESP_ERR_INVALID_ARG, why, why_len, "unknown key \"%s\"", item->string ? item->string : "");
        }
    }
    return ESP_OK;
}

esp_err_t playset_from_json(const cJSON *json, playset_t *out, char *why, size_t why_len)
{
    static const char *const top_keys[] = {"name", "channels"};
    // "id" is accepted and ignored, so that what playset_to_json() gives can be sent back.
    static const char *const channel_keys[] = {"source", "kind", "identifier", "display_name", "weight", "offset", "id"};
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(json)) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "a playset is a JSON object");
    }
    esp_err_t err = refuse_unknown_keys(json, top_keys, 2, why, why_len);
    if (err == ESP_OK) {
        err = take_text(cJSON_GetObjectItemCaseSensitive(json, "name"), "name", out->name, sizeof(out->name), why,
                        why_len);
    }
    if (err != ESP_OK) {
        return err;
    }
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(json, "channels");
    if (!cJSON_IsArray(channels)) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "\"channels\" must be a list");
    }
    const int count = cJSON_GetArraySize(channels);
    if (count < 1 || count > PLAYSET_MAX_CHANNELS) {
        return fail(ESP_ERR_INVALID_ARG, why, why_len, "a playset has 1 to %d channels", PLAYSET_MAX_CHANNELS);
    }
    uint32_t at = 0;
    for (const cJSON *item = channels->child; item != NULL; item = item->next, at++) {
        playset_channel_t *ch = &out->channels[at];
        if (!cJSON_IsObject(item)) {
            return fail(ESP_ERR_INVALID_ARG, why, why_len, "channel %u is not an object", (unsigned)at);
        }
        err = refuse_unknown_keys(item, channel_keys, sizeof(channel_keys) / sizeof(channel_keys[0]), why, why_len);
        if (err != ESP_OK) {
            return err;
        }
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(item, "source");
        ch->source = cJSON_IsString(source) ? playset_source_parse(source->valuestring) : 0;
        if (ch->source == 0) {
            return fail(ESP_ERR_INVALID_ARG, why, why_len,
                        "channel %u: \"source\" must be local, makapix, giphy, klipy, museum or pinned", (unsigned)at);
        }
        uint32_t weight = 0, offset = 0;  // locals: the struct is packed
        if ((err = take_text(cJSON_GetObjectItemCaseSensitive(item, "kind"), "kind", ch->kind, sizeof(ch->kind), why,
                             why_len)) != ESP_OK ||
            (err = take_text(cJSON_GetObjectItemCaseSensitive(item, "identifier"), "identifier", ch->identifier,
                             sizeof(ch->identifier), why, why_len)) != ESP_OK ||
            (err = take_text(cJSON_GetObjectItemCaseSensitive(item, "display_name"), "display_name",
                             ch->display_name, sizeof(ch->display_name), why, why_len)) != ESP_OK ||
            (err = take_number(cJSON_GetObjectItemCaseSensitive(item, "weight"), "weight", PLAYSET_MAX_WEIGHT,
                               &weight, why, why_len)) != ESP_OK ||
            (err = take_number(cJSON_GetObjectItemCaseSensitive(item, "offset"), "offset", UINT32_MAX, &offset, why,
                               why_len)) != ESP_OK) {
            memset(out, 0, sizeof(*out));
            return err;
        }
        ch->weight = weight;
        ch->offset = offset;
    }
    out->count = (uint32_t)count;
    return playset_check(out, why, why_len);
}

cJSON *playset_to_json(const playset_t *playset)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *channels = root ? cJSON_AddArrayToObject(root, "channels") : NULL;
    if (channels == NULL || cJSON_AddStringToObject(root, "name", playset->name) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (uint32_t i = 0; i < playset->count && i < PLAYSET_MAX_CHANNELS; i++) {
        const playset_channel_t *ch = &playset->channels[i];
        char id[PLAYSET_CHANNEL_ID_LEN];
        playset_channel_id(ch, id);
        const char *source = playset_source_name(ch->source);
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(channels, item) ||
            !cJSON_AddStringToObject(item, "id", id) ||
            !cJSON_AddStringToObject(item, "source", source ? source : "") ||
            !cJSON_AddStringToObject(item, "kind", ch->kind) ||
            !cJSON_AddStringToObject(item, "identifier", ch->identifier) ||
            !cJSON_AddStringToObject(item, "display_name", ch->display_name) ||
            !cJSON_AddNumberToObject(item, "weight", ch->weight) ||
            !cJSON_AddNumberToObject(item, "offset", ch->offset)) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

// --- channel id -----------------------------------------------------------------

static uint64_t fnv1a(uint64_t h, const void *bytes, size_t len)
{
    const uint8_t *p = bytes;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

void playset_channel_id(const playset_channel_t *channel, char out[PLAYSET_CHANNEL_ID_LEN])
{
    // The canonical form: the source's number, then kind and identifier each
    // with its terminator (so "ab"+"c" and "a"+"bc" differ), then the offset
    // as four little-endian bytes.
    uint64_t h = 14695981039346656037ull;
    h = fnv1a(h, &channel->source, 1);
    h = fnv1a(h, channel->kind, strnlen(channel->kind, sizeof(channel->kind) - 1) + 1);
    h = fnv1a(h, channel->identifier, strnlen(channel->identifier, sizeof(channel->identifier) - 1) + 1);
    const uint8_t offset[4] = {(uint8_t)channel->offset, (uint8_t)(channel->offset >> 8),
                               (uint8_t)(channel->offset >> 16), (uint8_t)(channel->offset >> 24)};
    h = fnv1a(h, offset, sizeof(offset));
    snprintf(out, PLAYSET_CHANNEL_ID_LEN, "%08x%08x", (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFu));
}

// --- the packed form ------------------------------------------------------------

uint32_t playset_pack(const playset_t *playset, void *records)
{
    playset_header_record_t header = {0};
    memcpy(header.name, playset->name, sizeof(header.name));
    memcpy(records, &header, sizeof(header));
    memcpy((uint8_t *)records + PLAYSET_RECORD_SIZE, playset->channels,
           (size_t)playset->count * sizeof(playset_channel_t));
    return playset->count + 1;
}

esp_err_t playset_unpack(const void *records, uint32_t record_count, playset_t *out)
{
    memset(out, 0, sizeof(*out));
    if (records == NULL || record_count < 2 || record_count > PLAYSET_MAX_CHANNELS + 1) {
        return P3A_ERR_CORRUPT;
    }
    playset_header_record_t header;
    memcpy(&header, records, sizeof(header));
    if (!all_zero(header.reserved, sizeof(header.reserved))) {
        return P3A_ERR_CORRUPT;
    }
    memcpy(out->name, header.name, sizeof(out->name));
    out->count = record_count - 1;
    memcpy(out->channels, (const uint8_t *)records + PLAYSET_RECORD_SIZE,
           (size_t)out->count * sizeof(playset_channel_t));
    esp_err_t err = playset_check(out, NULL, 0);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        // Whatever is wrong with a file's contents, the file is what is wrong.
        return P3A_ERR_CORRUPT;
    }
    return ESP_OK;
}
