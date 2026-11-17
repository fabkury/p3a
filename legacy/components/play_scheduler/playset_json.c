// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file playset_json.c
 * @brief JSON ↔ ps_playset_t conversion implementation
 */

#include "playset_json.h"
#include "play_scheduler_internal.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "playset_json";

// ---------- String ↔ Enum Parsers ----------

ps_channel_type_t playset_parse_channel_type(const char *type_str)
{
    if (!type_str) return PS_CHANNEL_TYPE_NAMED;
    if (strcmp(type_str, "user") == 0) return PS_CHANNEL_TYPE_USER;
    if (strcmp(type_str, "reactions") == 0) return PS_CHANNEL_TYPE_REACTIONS;
    if (strcmp(type_str, "hashtag") == 0) return PS_CHANNEL_TYPE_HASHTAG;
    if (strcmp(type_str, "sdcard") == 0) return PS_CHANNEL_TYPE_SDCARD;
    if (strcmp(type_str, "giphy") == 0) return PS_CHANNEL_TYPE_GIPHY;
    if (strcmp(type_str, "klipy") == 0) return PS_CHANNEL_TYPE_KLIPY;
    if (strcmp(type_str, "institution") == 0) return PS_CHANNEL_TYPE_INSTITUTION;
    if (strcmp(type_str, "pinned") == 0) return PS_CHANNEL_TYPE_PINNED;
    return PS_CHANNEL_TYPE_NAMED;
}

// ---------- Enum → String Serializers ----------

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
    case PS_CHANNEL_TYPE_REACTIONS: return "reactions";
    case PS_CHANNEL_TYPE_HASHTAG: return "hashtag";
    case PS_CHANNEL_TYPE_SDCARD:  return "sdcard";
    case PS_CHANNEL_TYPE_GIPHY:   return "giphy";
    case PS_CHANNEL_TYPE_KLIPY:   return "klipy";
    case PS_CHANNEL_TYPE_ARTWORK: return "artwork";
    case PS_CHANNEL_TYPE_INSTITUTION: return "institution";
    case PS_CHANNEL_TYPE_PINNED:  return "pinned";
    case PS_CHANNEL_TYPE_NAMED:
    default:                      return "named";
    }
}

// ---------- High-level Functions ----------

esp_err_t playset_json_parse(const cJSON *json, ps_playset_t *out)
{
    if (!json || !out) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    // Parse playset name (optional)
    const cJSON *name_field = cJSON_GetObjectItemCaseSensitive(json, "name");
    if (cJSON_IsString(name_field)) {
        strlcpy(out->name, cJSON_GetStringValue(name_field), sizeof(out->name));
    }

    // pick_mode is now a global device setting (config_store). Older clients
    // may still include it in the payload — silently ignore.

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
            case PS_CHANNEL_TYPE_REACTIONS:
                strncpy(spec->name, "reactions", sizeof(spec->name) - 1);
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

        // Parse offset (per-playset starting slice into the channel's source).
        // Honored for INSTITUTION / GIPHY / SDCARD / PINNED; ignored at refresh
        // for other types. Modulo against the source's total count happens at
        // refresh-time, not here.
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(ch, "offset");
        if (cJSON_IsNumber(offset)) {
            double v = cJSON_GetNumberValue(offset);
            if (v < 0) v = 0;
            if (v > (double)UINT32_MAX) v = (double)UINT32_MAX;
            spec->offset = (uint32_t)v;
        }

        ps_ensure_display_name(spec);
    }

    return ESP_OK;
}

cJSON *playset_json_serialize(const ps_playset_t *playset)
{
    if (!playset) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    if (playset->name[0] != '\0') {
        cJSON_AddStringToObject(root, "name", playset->name);
    }

    cJSON *channels = cJSON_AddArrayToObject(root, "channels");
    if (!channels) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < playset->channel_count; i++) {
        const ps_channel_spec_t *spec = &playset->channels[i];

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
        if (spec->offset > 0) {
            cJSON_AddNumberToObject(ch, "offset", (double)spec->offset);
        }

        /* For ARTWORK channels, surface the embedded artwork metadata so the
           WebUI can render the now-playing title and (if the storage_key /
           art_url ever need to be displayed for diagnostics) the full payload.
           Only emit the sub-object when the channel is actually ARTWORK and
           at least one field is populated. */
        if (spec->type == PS_CHANNEL_TYPE_ARTWORK) {
            cJSON *aw = cJSON_AddObjectToObject(ch, "artwork");
            if (aw) {
                if (spec->artwork.post_id != 0) {
                    cJSON_AddNumberToObject(aw, "post_id", (double)spec->artwork.post_id);
                }
                if (spec->artwork.storage_key[0] != '\0') {
                    cJSON_AddStringToObject(aw, "storage_key", spec->artwork.storage_key);
                }
                if (spec->artwork.art_url[0] != '\0') {
                    cJSON_AddStringToObject(aw, "art_url", spec->artwork.art_url);
                }
                if (spec->artwork.filepath[0] != '\0') {
                    cJSON_AddStringToObject(aw, "filepath", spec->artwork.filepath);
                }
                if (spec->artwork.title[0] != '\0') {
                    cJSON_AddStringToObject(aw, "title", spec->artwork.title);
                }
            }
        }

        cJSON_AddItemToArray(channels, ch);
    }

    return root;
}
