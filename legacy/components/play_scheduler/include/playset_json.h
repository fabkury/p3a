// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playset_json.h
 * @brief JSON ↔ ps_playset_t conversion
 *
 * Shared module for parsing playsets from JSON (used by both MQTT/Makapix API
 * and HTTP REST endpoints) and serializing playsets to JSON (for CRUD read).
 */

#ifndef PLAYSET_JSON_H
#define PLAYSET_JSON_H

#include "esp_err.h"
#include "cJSON.h"
#include "play_scheduler_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------- String ↔ Enum Parsers ----------

ps_channel_type_t  playset_parse_channel_type(const char *type_str);

// ---------- Enum → String Serializers ----------

const char *playset_pick_mode_str(ps_pick_mode_t mode);
const char *playset_channel_type_str(ps_channel_type_t type);

// ---------- High-level Functions ----------

/**
 * @brief Parse a cJSON object into a ps_playset_t
 *
 * Expects: "channels" (array of objects with "type", "name", "identifier",
 * "display_name", "weight").
 *
 * The "channels" array is required and must have 1–PS_MAX_CHANNELS entries.
 * Unknown fields (such as a legacy "pick_mode" or "exposure_mode" from older
 * clients) are silently ignored — pick_mode is now a global device setting.
 *
 * @param json  cJSON object to parse (not modified)
 * @param out   Output playset (zeroed then populated)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on missing/invalid channels
 */
esp_err_t playset_json_parse(const cJSON *json, ps_playset_t *out);

/**
 * @brief Serialize a ps_playset_t to a cJSON object
 *
 * Creates a new cJSON object with the "channels" array. Caller owns the
 * returned object and must call cJSON_Delete() when done.
 *
 * @param playset  Playset to serialize
 * @return cJSON object on success, NULL on OOM
 */
cJSON *playset_json_serialize(const ps_playset_t *playset);

#ifdef __cplusplus
}
#endif

#endif // PLAYSET_JSON_H
