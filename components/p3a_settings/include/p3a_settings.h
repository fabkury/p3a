// SPDX-License-Identifier: Apache-2.0
// The settings store: everything a user can change, in one typed struct.
//
// The settings themselves are listed in components/p3a_settings/settings.json;
// the struct p3a_settings_t and the ids P3A_SET_* are generated from it. To
// add a setting, add an entry there. Nothing else needs to change for it to be
// stored, validated, and readable and writable through this API.
//
// How it works. The store reads NVS once at start, keeps the struct in RAM
// behind a mutex, and on a change writes every non-default value as one JSON
// blob. Reading is a struct copy and never touches flash. A write touches
// flash, so a caller fed by a slider settles on a value before it writes.
//
// What does not live here: Wi-Fi credentials and the Makapix registration
// (their owners keep them in their own NVS namespaces), anything that
// describes the build (Kconfig), and constants nobody changes (p3a_limits.h).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "p3a_settings_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up NVS (for every component, not only this one) and loads the
// settings. Call once, early in boot. The store is usable even when this
// fails: it then holds the defaults and nothing can be saved.
//
// Stored entries this firmware does not know, or no longer accepts, are
// ignored. A stored blob that cannot be read at all is kept aside under
// another key for a later look, and the defaults are used.
esp_err_t p3a_settings_start(void);

// A copy of every setting as of now.
void p3a_settings_get(p3a_settings_t *out);

// Changes settings from { key: value, ... }, all or nothing, and saves them.
//   ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_ARG   refused; `why` says which key and why
//   anything else                             could not be saved; nothing changed
// *changed (optional) has bit `id` set for each setting whose value changed.
// For each of those, P3A_EVENT_SETTING_CHANGED is emitted with the id.
esp_err_t p3a_settings_set_json(const cJSON *object, uint64_t *changed, char *why, size_t why_len);

// One setting at a time, for code that knows the type. Same errors.
esp_err_t p3a_settings_set_bool(p3a_setting_id_t id, bool value);
esp_err_t p3a_settings_set_int(p3a_setting_id_t id, int32_t value);
// Also for an enum (the value's name) and a colour ("#rrggbb").
esp_err_t p3a_settings_set_str(p3a_setting_id_t id, const char *value);

// Every setting as a new JSON object, secrets masked. The caller deletes it.
cJSON *p3a_settings_json(void);

// The schema itself (settings.json, as written), for the web UI, which draws
// the settings page from it. *len excludes the terminator.
const char *p3a_settings_schema(size_t *len);

// True once a setting marked "reboot" has changed during this boot.
bool p3a_settings_reboot_needed(void);

// Erases the whole NVS partition: settings, Wi-Fi credentials, the Makapix
// registration, recovery counters. The SD card is not touched. After this the
// store refuses changes; the caller restarts the device.
esp_err_t p3a_settings_factory_reset(void);

// --- diagnostics ----------------------------------------------------------------

typedef struct {
    bool loaded;           // a stored blob was found and read
    bool blob_unreadable;  // a stored blob was found but could not be parsed
    int skipped;           // stored entries ignored at load
    size_t stored_bytes;   // size of the blob as last read or written
} p3a_settings_stats_t;

p3a_settings_stats_t p3a_settings_stats(void);

#ifdef __cplusplus
}
#endif
