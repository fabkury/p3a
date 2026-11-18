// SPDX-License-Identifier: Apache-2.0
// The shape of one entry in the settings table. The table itself
// (p3a_setting_defs) and the typed struct (p3a_settings_t) are generated at
// build time from components/p3a_settings/settings.json by
// tools/gen_settings.py. To add or change a setting, edit that JSON file and
// nothing else.
//
// Host-testable.
#pragma once

#include <stdint.h>

#include "p3a_settings_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_SET_TYPE_BOOL,    // field: bool
    P3A_SET_TYPE_INT,     // field: int32_t, within min..max or one of choices
    P3A_SET_TYPE_STRING,  // field: char[max_len + 1], printable ASCII
    P3A_SET_TYPE_ENUM,    // field: uint8_t, an index into values
    P3A_SET_TYPE_COLOR,   // field: uint32_t 0xRRGGBB; "#rrggbb" in JSON
} p3a_setting_type_t;

// Extra validation for a string, by name.
typedef enum {
    P3A_SET_CHECK_NONE,
    P3A_SET_CHECK_DEVICE_NAME,  // empty, or a-z 0-9 '-', not starting or ending with '-'
    P3A_SET_CHECK_SD_ROOT,      // empty, or accepted by sd_path_validate_root()
    P3A_SET_CHECK_COUNTRY,      // empty, or two letters; stored in upper case
} p3a_setting_check_t;

#define P3A_SET_FLAG_REBOOT   0x01  // takes effect at the next boot
#define P3A_SET_FLAG_ADVANCED 0x02  // not shown on the settings page
#define P3A_SET_FLAG_SECRET   0x04  // never logged; masked when read back

typedef struct {
    const char *key;
    const char *group;
    const char *label;
    uint8_t type;   // p3a_setting_type_t
    uint8_t flags;  // P3A_SET_FLAG_*
    uint8_t check;  // p3a_setting_check_t
    uint8_t choice_count;
    uint8_t value_count;
    uint16_t offset;  // of the field in p3a_settings_t
    uint16_t size;    // of the field
    int32_t default_int;  // bool, int, enum index, colour
    int32_t min, max;     // int without choices
    const int32_t *choices;      // int with choices
    const char *const *values;   // enum
    const char *default_str;     // string
} p3a_setting_def_t;

extern const p3a_setting_def_t p3a_setting_defs[P3A_SET_COUNT];

#ifdef __cplusplus
}
#endif
