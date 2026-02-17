// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file playset_json.c
 * @brief JSON ↔ ps_scheduler_command_t conversion implementation
 */

#include "playset_json.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "playset_json";

// ---------- String ↔ Enum Parsers ----------

ps_exposure_mode_t playset_parse_exposure_mode(const char *mode_str)
{
    if (!mode_str) return PS_EXPOSURE_EQUAL;
    if (strcmp(mode_str, "manual") == 0) return PS_EXPOSURE_MANUAL;
    if (strcmp(mode_str, "proportional") == 0) return PS_EXPOSURE_PROPORTIONAL;
    return PS_EXPOSURE_EQUAL;
}

ps_pick_mode_t playset_parse_pick_mode(const char *mode_str)
{
    if (!mode_str) return PS_PICK_RECENCY;
    if (strcmp(mode_str, "random") == 0) return PS_PICK_RANDOM;
    return PS_PICK_RECENCY;
}

ps_channel_type_t playset_parse_channel_type(const char *type_str)
{
    if (!type_str) return PS_CHANNEL_TYPE_NAMED;
    if (strcmp(type_str, "user") == 0) return PS_CHANNEL_TYPE_USER;
    if (strcmp(type_str, "hashtag") == 0) return PS_CHANNEL_TYPE_HASHTAG;
    if (strcmp(type_str, "sdcard") == 0) return PS_CHANNEL_TYPE_SDCARD;
    if (strcmp(type_str, "giphy") == 0) return PS_CHANNEL_TYPE_GIPHY;
    return PS_CHANNEL_TYPE_NAMED;
}

// ---------- Enum → String Serializers ----------

const char *playset_exposure_mode_str(ps_exposure_mode_t mode)
{
    switch (mode) {
    case PS_EXPOSURE_MANUAL:       return "manual";
    case PS_EXPOSURE_PROPORTIONAL: return "proportional";
    case PS_EXPOSURE_EQUAL:
    default:                       return "equal";
    }
}

const char *playset_pick_mode_str(ps_pick_mode_t mode)
{
    switch (mode) {
    case PS_PICK_RANDOM: return "random";
    case PS_PICK_RECENCY:
    default:             return "recency";
    }
}

const char *playset_channel_type_str(ps_channel_type_t type)
{
    switch (type) {
    case PS_CHANNEL_TYPE_USER:    return "user";
    case PS_CHANNEL_TYPE_HASHTAG: return "hashtag";
    case PS_CHANNEL_TYPE_SDCARD:  return "sdcard";
    case PS_CHANNEL_TYPE_GIPHY:   return "giphy";
    case PS_CHANNEL_TYPE_ARTWORK: return "artwork";
    case PS_CHANNEL_TYPE_NAMED:
    default:                      return "named";
    }
}

// ---------- High-level Functions ----------

esp_err_t playset_json_parse(const cJSON *json, ps_scheduler_command_t *out)
{
    if (!json || !out) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    // Parse exposure_mode (optional, defaults to equal)
    const cJSON *exposure_mode = cJSON_GetObjectItemCaseSensitive(json, "exposure_mode");
    if (cJSON_IsString(exposure_mode)) {
        out->exposure_mode = playset_parse_exposure_mode(cJSON_GetStringValue(exposure_mode));
    } else {
        out->exposure_mode = PS_EXPOSURE_EQUAL;
    }

    // Parse pick_mode (optional, defaults to recency)
    const cJSON *pick_mode = cJSON_GetObjectItemCaseSensitive(json, "pick_mode");
    if (cJSON_IsString(pick_mode)) {
        out->pick_mode = playset_parse_pick_mode(cJSON_GetStringValue(pick_mode));
    } else {
        out->pick_mode = PS_PICK_RECENCY;
    }

    // Parse channels array (required)
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(json, "channels");
    if (!cJSON_IsArray(channels)) {
        ESP_LOGE(TAG, "Missing or invalid 'channels' array");
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = cJSON_GetArraySize(channels);
    if (count == 0 || count > PS_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel count %zu (must be 1-%d)", count, PS_MAX_CHANNELS);
        return ESP_ERR_INVALID_ARG;
    }

    out->channel_count = count;

    for (size_t i = 0; i < count; i++) {
        const cJSON *ch = cJSON_GetArrayItem(channels, (int)i);
        if (!cJSON_IsObject(ch)) continue;

        ps_channel_spec_t *spec = &out->channels[i];

        // Parse type
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(ch, "type");
        if (cJSON_IsString(type)) {
            spec->type = playset_parse_channel_type(cJSON_GetStringValue(type));
        } else {
            spec->type = PS_CHANNEL_TYPE_NAMED;
        }

        // Parse name
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(ch, "name");
        if (cJSON_IsString(name)) {
            strncpy(spec->name, cJSON_GetStringValue(name), sizeof(spec->name) - 1);
        } else {
            // Set default name based on type
            switch (spec->type) {
            case PS_CHANNEL_TYPE_USER:
                strncpy(spec->name, "user", sizeof(spec->name) - 1);
                break;
            case PS_CHANNEL_TYPE_HASHTAG:
                strncpy(spec->name, "hashtag", sizeof(spec->name) - 1);
                break;
            case PS_CHANNEL_TYPE_SDCARD:
                strncpy(spec->name, "sdcard", sizeof(spec->name) - 1);
                break;
            default:
                strncpy(spec->name, "all", sizeof(spec->name) - 1);
                break;
            }
        }

        // Parse identifier
        const cJSON *identifier = cJSON_GetObjectItemCaseSensitive(ch, "identifier");
        if (cJSON_IsString(identifier)) {
            strncpy(spec->identifier, cJSON_GetStringValue(identifier), sizeof(spec->identifier) - 1);
        }

        // Parse display_name
        const cJSON *display_name = cJSON_GetObjectItemCaseSensitive(ch, "display_name");
        if (cJSON_IsString(display_name)) {
            strncpy(spec->display_name, cJSON_GetStringValue(display_name), sizeof(spec->display_name) - 1);
        }

        // Parse weight
        const cJSON *weight = cJSON_GetObjectItemCaseSensitive(ch, "weight");
        if (cJSON_IsNumber(weight)) {
            spec->weight = (uint32_t)cJSON_GetNumberValue(weight);
        }
    }

    return ESP_OK;
}

cJSON *playset_json_serialize(const ps_scheduler_command_t *cmd)
{
    if (!cmd) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "exposure_mode", playset_exposure_mode_str(cmd->exposure_mode));
    cJSON_AddStringToObject(root, "pick_mode", playset_pick_mode_str(cmd->pick_mode));

    cJSON *channels = cJSON_AddArrayToObject(root, "channels");
    if (!channels) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < cmd->channel_count; i++) {
        const ps_channel_spec_t *spec = &cmd->channels[i];

        cJSON *ch = cJSON_CreateObject();
        if (!ch) continue;

        cJSON_AddStringToObject(ch, "type", playset_channel_type_str(spec->type));
        cJSON_AddStringToObject(ch, "name", spec->name);

        if (spec->identifier[0] != '\0') {
            cJSON_AddStringToObject(ch, "identifier", spec->identifier);
        }
        if (spec->display_name[0] != '\0') {
            cJSON_AddStringToObject(ch, "display_name", spec->display_name);
        }
        if (spec->weight > 0) {
            cJSON_AddNumberToObject(ch, "weight", (double)spec->weight);
        }

        cJSON_AddItemToArray(channels, ch);
    }

    return root;
}
