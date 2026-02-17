// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file playset_json.h
 * @brief JSON ↔ ps_scheduler_command_t conversion
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

ps_exposure_mode_t playset_parse_exposure_mode(const char *mode_str);
ps_pick_mode_t     playset_parse_pick_mode(const char *mode_str);
ps_channel_type_t  playset_parse_channel_type(const char *type_str);

// ---------- Enum → String Serializers ----------

const char *playset_exposure_mode_str(ps_exposure_mode_t mode);
const char *playset_pick_mode_str(ps_pick_mode_t mode);
const char *playset_channel_type_str(ps_channel_type_t type);

// ---------- High-level Functions ----------

/**
 * @brief Parse a cJSON object into a ps_scheduler_command_t
 *
 * Expects fields: "exposure_mode" (string), "pick_mode" (string),
 * "channels" (array of objects with "type", "name", "identifier",
 * "display_name", "weight").
 *
 * Missing top-level fields get defaults (equal exposure, recency pick).
 * The "channels" array is required and must have 1–PS_MAX_CHANNELS entries.
 *
 * @param json  cJSON object to parse (not modified)
 * @param out   Output scheduler command (zeroed then populated)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on missing/invalid channels
 */
esp_err_t playset_json_parse(const cJSON *json, ps_scheduler_command_t *out);

/**
 * @brief Serialize a ps_scheduler_command_t to a cJSON object
 *
 * Creates a new cJSON object with "exposure_mode", "pick_mode", and
 * "channels" array. Caller owns the returned object and must call
 * cJSON_Delete() when done.
 *
 * @param cmd  Scheduler command to serialize
 * @return cJSON object on success, NULL on OOM
 */
cJSON *playset_json_serialize(const ps_scheduler_command_t *cmd);

#ifdef __cplusplus
}
#endif

#endif // PLAYSET_JSON_H
