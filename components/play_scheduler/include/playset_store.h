// SPDX-License-Identifier: Apache-2.0
// Playsets on the SD card: the ones the user saved under a name, and the one
// that is active (so that the device plays the same thing after a restart).
//
//   {root}/playsets/{16 hex digits from the name}.p3af   a saved playset
//   {root}/playsets/active.p3af                          the active one
//
// Both are framed containers (p3a_frame.h) of the packed playset, flagged as
// user data: a file that fails its check is reported and left alone, never
// overwritten by a load and never deleted except by playset_store_delete().
// 1.x kept the two in two unrelated formats (audit M21).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "playset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYSET_STORE_MAX 32  // saved playsets; the web UI offers no more

typedef struct {
    char name[PLAYSET_NAME_LEN];
    uint32_t channel_count;
} playset_store_entry_t;

// Saves a playset under its name, replacing one of the same name.
//   ESP_ERR_INVALID_ARG    it has no name, or does not pass playset_check()
//   ESP_ERR_NO_MEM         PLAYSET_STORE_MAX are saved already and this would be one more
//   ESP_ERR_INVALID_STATE  the file for this name holds something that cannot be read,
//                          or another name's playset: it is not overwritten
//   and what a write to the card can return (P3A_ERR_SD_FAILED among them)
esp_err_t playset_store_save(const playset_t *playset);

// P3A_ERR_MISSING when there is none of that name; P3A_ERR_CORRUPT,
// ESP_ERR_INVALID_VERSION and the rest as p3a_frame_load() reports them.
esp_err_t playset_store_load(const char *name, playset_t *out);

// Removes a saved playset because the user asked. P3A_ERR_MISSING if there is none.
esp_err_t playset_store_delete(const char *name);

// The saved playsets, in no particular order. Files that cannot be read are
// counted in *unreadable (if given) and left where they are.
esp_err_t playset_store_list(playset_store_entry_t *out, uint32_t max, uint32_t *count, uint32_t *unreadable);

// The file a name's playset is kept in, whether or not it exists.
esp_err_t playset_store_path(const char *name, char *out, size_t len);

// The active playset. It need not have a name.
esp_err_t playset_store_save_active(const playset_t *playset);
esp_err_t playset_store_load_active(playset_t *out);

#ifdef __cplusplus
}
#endif
