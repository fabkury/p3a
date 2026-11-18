// SPDX-License-Identifier: Apache-2.0
// Settings as data. See p3a_settings_model.h. No IDF includes: this file is
// also compiled by the host tests.

#include "p3a_settings_model.h"

#include <stdio.h>
#include <string.h>

#include "sd_path.h"

static void *field(p3a_settings_t *s, const p3a_setting_def_t *d)
{
    return (char *)s + d->offset;
}

static const void *cfield(const p3a_settings_t *s, const p3a_setting_def_t *d)
{
    return (const char *)s + d->offset;
}

static void set_default(p3a_settings_t *s, const p3a_setting_def_t *d)
{
    void *f = field(s, d);
    switch (d->type) {
    case P3A_SET_TYPE_BOOL:
        *(bool *)f = d->default_int != 0;
        break;
    case P3A_SET_TYPE_INT:
        *(int32_t *)f = d->default_int;
        break;
    case P3A_SET_TYPE_ENUM:
        *(uint8_t *)f = (uint8_t)d->default_int;
        break;
    case P3A_SET_TYPE_COLOR:
        *(uint32_t *)f = (uint32_t)d->default_int;
        break;
    default:
        memset(f, 0, d->size);
        snprintf((char *)f, d->size, "%s", d->default_str);
        break;
    }
}

void p3a_settings_defaults(p3a_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        set_default(s, &p3a_setting_defs[i]);
    }
}

int p3a_settings_find(const char *key)
{
    if (key != NULL) {
        for (int i = 0; i < P3A_SET_COUNT; i++) {
            if (strcmp(key, p3a_setting_defs[i].key) == 0) {
                return i;
            }
        }
    }
    return -1;
}

// Compares one field of two structs. Strings compare as strings, so bytes
// after the terminator do not matter.
static bool same(const p3a_settings_t *a, const p3a_settings_t *b, const p3a_setting_def_t *d)
{
    if (d->type == P3A_SET_TYPE_STRING) {
        return strcmp((const char *)cfield(a, d), (const char *)cfield(b, d)) == 0;
    }
    return memcmp(cfield(a, d), cfield(b, d), d->size) == 0;
}

bool p3a_settings_is_default(const p3a_settings_t *s, p3a_setting_id_t id)
{
    if ((int)id < 0 || id >= P3A_SET_COUNT) {
        return false;
    }
    const p3a_setting_def_t *d = &p3a_setting_defs[id];
    const void *f = cfield(s, d);
    switch (d->type) {
    case P3A_SET_TYPE_BOOL:
        return *(const bool *)f == (d->default_int != 0);
    case P3A_SET_TYPE_INT:
        return *(const int32_t *)f == d->default_int;
    case P3A_SET_TYPE_ENUM:
        return *(const uint8_t *)f == (uint8_t)d->default_int;
    case P3A_SET_TYPE_COLOR:
        return *(const uint32_t *)f == (uint32_t)d->default_int;
    default:
        return strcmp((const char *)f, d->default_str) == 0;
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool device_name_ok(const char *v)
{
    size_t len = strlen(v);
    if (len == 0) {
        return true;
    }
    if (v[0] == '-' || v[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// Validates a string value and writes it to the field. Returns NULL, or what
// is wrong with it.
static const char *set_string(p3a_settings_t *s, const p3a_setting_def_t *d, const char *v)
{
    char *f = field(s, d);
    if ((d->flags & P3A_SET_FLAG_SECRET) && strcmp(v, P3A_SETTINGS_SECRET_MASK) == 0) {
        return NULL;  // the mask written back: keep the stored secret
    }
    size_t len = strlen(v);
    if (len >= d->size) {
        return "is too long";
    }
    for (size_t i = 0; i < len; i++) {
        if (v[i] < 0x20 || v[i] > 0x7e) {
            return "may only contain plain printable characters";
        }
    }
    switch (d->check) {
    case P3A_SET_CHECK_DEVICE_NAME:
        if (!device_name_ok(v)) {
            return "may only contain a-z, 0-9 and '-', and may not start or end with '-'";
        }
        break;
    case P3A_SET_CHECK_SD_ROOT:
        if (len > 0 && sd_path_validate_root(v) != ESP_OK) {
            return "must be a folder path such as /p3a";
        }
        break;
    case P3A_SET_CHECK_COUNTRY:
        if (len > 0 && !(len == 2 && is_letter(v[0]) && is_letter(v[1]))) {
            return "must be a two-letter country code, or empty";
        }
        break;
    default:
        break;
    }
    memset(f, 0, d->size);
    memcpy(f, v, len);
    if (d->check == P3A_SET_CHECK_COUNTRY) {
        for (size_t i = 0; i < len; i++) {
            if (f[i] >= 'a' && f[i] <= 'z') {
                f[i] = (char)(f[i] - 'a' + 'A');
            }
        }
    }
    return NULL;
}

// Validates one JSON value and writes it to its field. Returns NULL, or what
// is wrong with it. The field is written only on success.
static const char *set_one(p3a_settings_t *s, const p3a_setting_def_t *d, const cJSON *v)
{
    void *f = field(s, d);
    switch (d->type) {
    case P3A_SET_TYPE_BOOL:
        if (!cJSON_IsBool(v)) {
            return "must be true or false";
        }
        *(bool *)f = cJSON_IsTrue(v);
        return NULL;

    case P3A_SET_TYPE_INT: {
        if (!cJSON_IsNumber(v)) {
            return "must be a number";
        }
        double n = v->valuedouble;
        if (!(n >= -2147483648.0 && n <= 2147483647.0) || n != (double)(int32_t)n) {
            return "must be a whole number";
        }
        int32_t i = (int32_t)n;
        if (d->choice_count > 0) {
            bool found = false;
            for (int c = 0; c < d->choice_count; c++) {
                found = found || d->choices[c] == i;
            }
            if (!found) {
                return "is not one of the allowed values";
            }
        } else if (i < d->min || i > d->max) {
            return "is out of range";
        }
        *(int32_t *)f = i;
        return NULL;
    }

    case P3A_SET_TYPE_ENUM:
        if (!cJSON_IsString(v)) {
            return "must be a string";
        }
        for (int c = 0; c < d->value_count; c++) {
            if (strcmp(v->valuestring, d->values[c]) == 0) {
                *(uint8_t *)f = (uint8_t)c;
                return NULL;
            }
        }
        return "is not one of the allowed values";

    case P3A_SET_TYPE_COLOR: {
        if (!cJSON_IsString(v)) {
            return "must be a string like #1a2b3c";
        }
        const char *t = v->valuestring;
        if (strlen(t) != 7 || t[0] != '#') {
            return "must look like #1a2b3c";
        }
        uint32_t rgb = 0;
        for (int i = 1; i < 7; i++) {
            int h = hex_digit(t[i]);
            if (h < 0) {
                return "must look like #1a2b3c";
            }
            rgb = (rgb << 4) | (uint32_t)h;
        }
        *(uint32_t *)f = rgb;
        return NULL;
    }

    default:
        if (!cJSON_IsString(v)) {
            return "must be a string";
        }
        return set_string(s, d, v->valuestring);
    }
}

static void explain(char *why, size_t why_len, const char *key, const char *problem)
{
    if (why != NULL && why_len > 0) {
        // The key comes from the caller: show only a bounded, printable part.
        char shown[32];
        size_t n = 0;
        for (; key != NULL && key[n] != '\0' && n < sizeof(shown) - 1; n++) {
            shown[n] = (key[n] >= 0x20 && key[n] <= 0x7e && key[n] != '"') ? key[n] : '?';
        }
        shown[n] = '\0';
        snprintf(why, why_len, "%s %s", shown, problem);
    }
}

esp_err_t p3a_settings_apply_json(p3a_settings_t *s, const cJSON *object, uint64_t *changed, char *why,
                                  size_t why_len)
{
    if (changed != NULL) {
        *changed = 0;
    }
    if (s == NULL || !cJSON_IsObject(object)) {
        explain(why, why_len, "settings", "must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    p3a_settings_t next = *s;
    const cJSON *item;
    cJSON_ArrayForEach(item, object)
    {
        int id = p3a_settings_find(item->string);
        if (id < 0) {
            explain(why, why_len, item->string, "is not a setting");
            return ESP_ERR_NOT_FOUND;
        }
        const char *problem = set_one(&next, &p3a_setting_defs[id], item);
        if (problem != NULL) {
            explain(why, why_len, item->string, problem);
            return ESP_ERR_INVALID_ARG;
        }
    }
    uint64_t mask = 0;
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        if (!same(s, &next, &p3a_setting_defs[i])) {
            mask |= 1ULL << i;
        }
    }
    *s = next;
    if (changed != NULL) {
        *changed = mask;
    }
    return ESP_OK;
}

int p3a_settings_load_json(p3a_settings_t *s, const cJSON *object)
{
    if (s == NULL || !cJSON_IsObject(object)) {
        return 0;
    }
    int skipped = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, object)
    {
        int id = p3a_settings_find(item->string);
        const p3a_setting_def_t *d = id >= 0 ? &p3a_setting_defs[id] : NULL;
        // A stored secret is never the mask; refuse it rather than read it as "keep".
        bool mask = d != NULL && (d->flags & P3A_SET_FLAG_SECRET) && cJSON_IsString(item) &&
                    strcmp(item->valuestring, P3A_SETTINGS_SECRET_MASK) == 0;
        if (d == NULL || mask || set_one(s, d, item) != NULL) {
            skipped++;
        }
    }
    return skipped;
}

cJSON *p3a_settings_to_json(const p3a_settings_t *s, unsigned flags)
{
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) {
        return NULL;
    }
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        const p3a_setting_def_t *d = &p3a_setting_defs[i];
        if ((flags & P3A_SETTINGS_JSON_ONLY_CHANGED) && p3a_settings_is_default(s, (p3a_setting_id_t)i)) {
            continue;
        }
        const void *f = cfield(s, d);
        cJSON *added;
        switch (d->type) {
        case P3A_SET_TYPE_BOOL:
            added = cJSON_AddBoolToObject(object, d->key, *(const bool *)f);
            break;
        case P3A_SET_TYPE_INT:
            added = cJSON_AddNumberToObject(object, d->key, (double)*(const int32_t *)f);
            break;
        case P3A_SET_TYPE_ENUM: {
            uint8_t index = *(const uint8_t *)f;
            added = cJSON_AddStringToObject(object, d->key, d->values[index < d->value_count ? index : 0]);
            break;
        }
        case P3A_SET_TYPE_COLOR: {
            char text[8];
            snprintf(text, sizeof(text), "#%06x", (unsigned)(*(const uint32_t *)f & 0xFFFFFFu));
            added = cJSON_AddStringToObject(object, d->key, text);
            break;
        }
        default: {
            const char *text = (const char *)f;
            if ((flags & P3A_SETTINGS_JSON_MASK_SECRETS) && (d->flags & P3A_SET_FLAG_SECRET) && text[0] != '\0') {
                text = P3A_SETTINGS_SECRET_MASK;
            }
            added = cJSON_AddStringToObject(object, d->key, text);
            break;
        }
        }
        if (added == NULL) {
            cJSON_Delete(object);
            return NULL;
        }
    }
    return object;
}
