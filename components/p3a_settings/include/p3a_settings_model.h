// SPDX-License-Identifier: Apache-2.0
// Settings as data: defaults, validation, and conversion to and from JSON.
// No storage and no locking here; p3a_settings.h is the store built on top.
// Most code wants that header, not this one.
//
// In JSON a setting is { "key": value }: true/false for a bool, a whole
// number for an int, a string for a string, one of the listed strings for an
// enum, "#rrggbb" for a colour.
//
// Host-testable.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "p3a_settings_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a secret that has a value reads back as. Writing this exact string to
// a secret leaves it as it is, so a form can send back what it was given.
#define P3A_SETTINGS_SECRET_MASK "********"

// Fills *s with every default.
void p3a_settings_defaults(p3a_settings_t *s);

// The id of a key, or -1.
int p3a_settings_find(const char *key);

// True when the setting holds its default in *s.
bool p3a_settings_is_default(const p3a_settings_t *s, p3a_setting_id_t id);

// Applies { key: value, ... } to *s, all or nothing: if any entry is refused,
// *s is untouched.
//   ESP_ERR_NOT_FOUND     a key that is not a setting
//   ESP_ERR_INVALID_ARG   a value of the wrong type, out of range, or malformed
// On failure `why` (optional) receives a sentence that names the key, fit to
// show to a user. On success *changed (optional) has bit `id` set for every
// setting whose value is now different.
esp_err_t p3a_settings_apply_json(p3a_settings_t *s, const cJSON *object, uint64_t *changed, char *why,
                                  size_t why_len);

// The forgiving version, for what was stored by another firmware version:
// entries that are unknown or no longer valid are skipped and the rest are
// applied. Returns how many were skipped.
int p3a_settings_load_json(p3a_settings_t *s, const cJSON *object);

#define P3A_SETTINGS_JSON_ONLY_CHANGED 0x01  // leave out settings that hold their default
#define P3A_SETTINGS_JSON_MASK_SECRETS 0x02  // a secret reads as P3A_SETTINGS_SECRET_MASK, or "" when unset

// A new JSON object holding the settings. The caller deletes it. NULL when
// out of memory.
cJSON *p3a_settings_to_json(const p3a_settings_t *s, unsigned flags);

#ifdef __cplusplus
}
#endif
