// SPDX-License-Identifier: Apache-2.0
// What a command from Makapix Club asks for. See makapix_command_map.h. Pure.

#include "makapix_command_map.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psram_alloc.h"
#include "show_url.h"
#include "source_local.h"
#include "source_makapix.h"

static const char *text_of(const cJSON *object, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// A whole number within [lo, hi], or `otherwise`.
static int64_t whole(const cJSON *object, const char *name, int64_t lo, int64_t hi, int64_t otherwise)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(v) || v->valuedouble < (double)lo || v->valuedouble > (double)hi ||
        v->valuedouble != (double)(int64_t)v->valuedouble) {
        return otherwise;
    }
    return (int64_t)v->valuedouble;
}

static void refuse(makapix_action_t *out, const char *format, ...)
{
    out->kind = MAKAPIX_DO_REFUSE;
    va_list args;
    va_start(args, format);
    vsnprintf(out->text, sizeof(out->text), format, args);
    va_end(args);
}

static bool fits(const char *text, size_t room)
{
    return text != NULL && strlen(text) < room;
}

// One channel of a playset being built. False when the text does not fit.
static bool add_channel(playset_t *playset, uint8_t source, const char *kind, const char *identifier,
                        const char *display_name, uint32_t weight)
{
    if (playset->count >= PLAYSET_MAX_CHANNELS || !fits(kind != NULL ? kind : "", PLAYSET_KIND_LEN) ||
        !fits(identifier != NULL ? identifier : "", PLAYSET_IDENT_LEN) ||
        !fits(display_name != NULL ? display_name : "", PLAYSET_DISPLAY_LEN)) {
        return false;
    }
    playset_channel_t *ch = &playset->channels[playset->count++];
    memset(ch, 0, sizeof(*ch));
    ch->source = source;
    ch->weight = weight;
    snprintf(ch->kind, sizeof(ch->kind), "%s", kind != NULL ? kind : "");
    snprintf(ch->identifier, sizeof(ch->identifier), "%s", identifier != NULL ? identifier : "");
    snprintf(ch->display_name, sizeof(ch->display_name), "%s", display_name != NULL ? display_name : "");
    return true;
}

// The playset must pass the scheduler's own check before it is handed on.
static void finish_playset(makapix_action_t *out)
{
    char why[120] = "";
    if (playset_check(out->playset, why, sizeof(why)) != ESP_OK) {
        makapix_action_free(out);
        refuse(out, "%s", why);
    } else {
        out->kind = MAKAPIX_DO_PLAYSET;
    }
}

static void map_play_channel(const cJSON *payload, makapix_action_t *out)
{
    const char *name = text_of(payload, "channel_name");
    if (name == NULL) {
        refuse(out, "play_channel needs \"channel_name\"");
        return;
    }
    out->playset = psram_calloc(1, sizeof(playset_t));
    if (out->playset == NULL) {
        refuse(out, "out of memory");
        return;
    }
    bool ok = false;
    if (strcmp(name, "all") == 0 || strcmp(name, "promoted") == 0) {
        ok = add_channel(out->playset, PLAYSET_SOURCE_MAKAPIX, name, NULL, NULL, 0);
    } else if (strcmp(name, "sdcard") == 0) {
        ok = add_channel(out->playset, PLAYSET_SOURCE_LOCAL, NULL, NULL, NULL, 0);
    } else if (strcmp(name, "by_user") == 0 || strcmp(name, "reactions") == 0) {
        const char *sqid = text_of(payload, "user_sqid");
        ok = sqid != NULL && sqid[0] != '\0' && add_channel(out->playset, PLAYSET_SOURCE_MAKAPIX, name, sqid, NULL, 0);
    } else if (strcmp(name, "hashtag") == 0) {
        const char *tag = text_of(payload, "hashtag");
        ok = tag != NULL && tag[0] != '\0' && add_channel(out->playset, PLAYSET_SOURCE_MAKAPIX, name, tag, NULL, 0);
    } else if (strcmp(name, "pinned") == 0) {
        ok = add_channel(out->playset, PLAYSET_SOURCE_PINNED, NULL, text_of(payload, "slug"), NULL, 0);
    }
    if (!ok) {
        makapix_action_free(out);
        refuse(out, "play_channel: \"%.40s\" is not a channel this device can play, or what it needs is missing", name);
        return;
    }
    finish_playset(out);
}

static void map_playset(const cJSON *payload, makapix_action_t *out)
{
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(payload, "channels");
    if (!cJSON_IsArray(channels) || cJSON_GetArraySize(channels) == 0) {
        refuse(out, "a playset needs \"channels\"");
        return;
    }
    out->playset = psram_calloc(1, sizeof(playset_t));
    if (out->playset == NULL) {
        refuse(out, "out of memory");
        return;
    }
    // In p3a's own form (every channel names its "source") it is read by
    // the playset's own reader, which checks everything.
    const cJSON *first = cJSON_GetArrayItem(channels, 0);
    if (cJSON_IsObject(first) && cJSON_GetObjectItemCaseSensitive(first, "source") != NULL) {
        cJSON *own = cJSON_CreateObject();
        const char *name = text_of(payload, "name");
        cJSON_AddItemReferenceToObject(own, "channels", (cJSON *)channels);
        if (name != NULL) {
            cJSON_AddStringToObject(own, "name", name);
        }
        char why[120] = "";
        const esp_err_t err = playset_from_json(own, out->playset, why, sizeof(why));
        cJSON_Delete(own);
        if (err != ESP_OK) {
            makapix_action_free(out);
            refuse(out, "%s", why);
            return;
        }
        out->save = out->playset->name[0] != '\0';
        out->kind = MAKAPIX_DO_PLAYSET;
        return;
    }
    // The server's form.
    const cJSON *channel = NULL;
    cJSON_ArrayForEach(channel, channels)
    {
        const char *type = cJSON_IsObject(channel) ? text_of(channel, "type") : NULL;
        const char *name = text_of(channel, "name");
        const char *identifier = text_of(channel, "identifier");
        const char *display = text_of(channel, "display_name");
        const uint32_t weight = (uint32_t)whole(channel, "weight", 0, PLAYSET_MAX_WEIGHT, 0);
        bool ok = false;
        if (type == NULL) {
            ok = false;
        } else if (strcmp(type, "named") == 0) {
            ok = name != NULL && (strcmp(name, "all") == 0 || strcmp(name, "promoted") == 0) &&
                 add_channel(out->playset, PLAYSET_SOURCE_MAKAPIX, name, NULL, display, weight);
        } else if (strcmp(type, "user") == 0) {
            ok = identifier != NULL && identifier[0] != '\0' &&
                 add_channel(out->playset, PLAYSET_SOURCE_MAKAPIX, "by_user", identifier, display, weight);
        } else if (strcmp(type, "hashtag") == 0) {
            ok = identifier != NULL && identifier[0] != '\0' &&
                 add_channel(out->playset, PLAYSET_SOURCE_MAKAPIX, "hashtag", identifier, display, weight);
        } else if (strcmp(type, "sdcard") == 0) {
            ok = add_channel(out->playset, PLAYSET_SOURCE_LOCAL, NULL, NULL, display, weight);
        }
        if (!ok) {
            makapix_action_free(out);
            refuse(out, "a playset channel of type \"%.24s\" cannot be played, or what it needs is missing",
                   type != NULL ? type : "(none)");
            return;
        }
    }
    // 1.x's command gave "name" to save the playset as well; the server's
    // gives "playset_name", which only says what it is.
    const char *save_as = text_of(payload, "name");
    const char *called = save_as != NULL ? save_as : text_of(payload, "playset_name");
    if (called != NULL && fits(called, PLAYSET_NAME_LEN)) {
        snprintf(out->playset->name, sizeof(out->playset->name), "%s", called);
    }
    out->save = save_as != NULL && out->playset->name[0] != '\0';
    finish_playset(out);
}

static void map_artwork(const cJSON *payload, bool with_url, makapix_action_t *out)
{
    const uint32_t post_id = (uint32_t)whole(payload, "post_id", 1, UINT32_MAX, 0);
    const char *key = text_of(payload, "storage_key");
    const esp_err_t err = source_makapix_record(post_id, key, with_url ? text_of(payload, "art_url") : NULL,
                                                with_url ? NULL : text_of(payload, "native_format"), &out->record);
    if (err != ESP_OK) {
        refuse(out, "an artwork is named by \"post_id\", \"storage_key\" and %s",
               with_url ? "an \"art_url\" ending in its format" : "\"native_format\"");
        return;
    }
    const uint32_t w = (uint32_t)whole(payload, "width", 1, 4096, 0), h = (uint32_t)whole(payload, "height", 1, 4096, 0);
    if (w != 0 && h != 0) {
        out->record.width = (uint16_t)w;
        out->record.height = (uint16_t)h;
    }
    out->kind = MAKAPIX_DO_SHOW;
}

void makapix_command_map(const char *type, const cJSON *payload, makapix_action_t *out)
{
    memset(out, 0, sizeof(*out));
    if (type == NULL) {
        return;
    }
    if (strcmp(type, "swap_next") == 0) {
        out->kind = MAKAPIX_DO_NEXT;
    } else if (strcmp(type, "swap_back") == 0) {
        out->kind = MAKAPIX_DO_BACK;
    } else if (strcmp(type, "show_artwork") == 0) {
        map_artwork(payload, false, out);
    } else if (strcmp(type, "swap_to") == 0) {
        const char *channel = text_of(payload, "channel");
        if (channel != NULL && strcmp(channel, "sdcard") == 0) {
            const char *filename = text_of(payload, "filename");
            // A name, not a path, and a picture's.
            if (filename == NULL || !fits(filename, P3A_RECORD_FILENAME_LEN) || strpbrk(filename, "/\\") != NULL ||
                source_local_format_of(filename) == P3A_FORMAT_UNKNOWN) {
                refuse(out, "swap_to: \"filename\" must name a picture in the user's folder");
            } else {
                out->kind = MAKAPIX_DO_SHOW_FILE;
                snprintf(out->text, sizeof(out->text), "%s", filename);
            }
        } else {
            map_artwork(payload, true, out);
        }
    } else if (strcmp(type, "show_url") == 0) {
        const char *url = text_of(payload, "artwork_url");
        if (!show_url_ok(url)) {
            refuse(out, "show_url: \"artwork_url\" must be an http:// or https:// address");
        } else {
            out->kind = MAKAPIX_DO_SHOW_URL;
            snprintf(out->text, sizeof(out->text), "%s", url);
        }
    } else if (strcmp(type, "play_channel") == 0) {
        map_play_channel(payload, out);
    } else if (strcmp(type, "play_playset") == 0 || strcmp(type, "execute_playset") == 0) {
        map_playset(payload, out);
    } else if (strcmp(type, "set_background_color") == 0) {
        const int64_t r = whole(payload, "r", 0, 255, -1), g = whole(payload, "g", 0, 255, -1), b = whole(payload, "b", 0, 255, -1);
        if (r < 0 || g < 0 || b < 0) {
            refuse(out, "set_background_color needs \"r\", \"g\" and \"b\", each 0 to 255");
        } else {
            out->kind = MAKAPIX_DO_BACKGROUND;
            snprintf(out->text, sizeof(out->text), "#%02X%02X%02X", (unsigned)r, (unsigned)g, (unsigned)b);
        }
    } else if (strcmp(type, "set_paused") == 0) {
        out->wants_ack = true;
        const cJSON *paused = cJSON_GetObjectItemCaseSensitive(payload, "paused");
        if (!cJSON_IsBool(paused)) {
            refuse(out, "missing or invalid 'paused' field");
        } else {
            out->kind = MAKAPIX_DO_PAUSE;
            out->value = cJSON_IsTrue(paused) ? 1 : 0;
        }
    } else if (strcmp(type, "set_brightness") == 0) {
        out->wants_ack = true;
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(payload, "value");
        if (!cJSON_IsNumber(value)) {
            refuse(out, "missing or invalid 'value' field");
        } else {
            out->kind = MAKAPIX_DO_BRIGHTNESS;  // out of range is brought into it, as 1.x did
            out->value = value->valuedouble < 0 ? 0 : value->valuedouble > 100 ? 100 : (int32_t)value->valuedouble;
        }
    } else if (strcmp(type, "set_rotation") == 0) {
        out->wants_ack = true;
        const int64_t value = whole(payload, "value", 0, 270, -1);
        if (value != 0 && value != 90 && value != 180 && value != 270) {
            refuse(out, "rotation must be 0, 90, 180, or 270");
        } else {
            out->kind = MAKAPIX_DO_ROTATION;
            out->value = (int32_t)value;
        }
    } else if (strcmp(type, "set_mirror") == 0) {
        out->wants_ack = true;
        out->kind = MAKAPIX_DO_UNSUPPORTED;
    }
}

void makapix_action_free(makapix_action_t *action)
{
    free(action->playset);
    action->playset = NULL;
}

const char *makapix_do_name(makapix_do_t kind)
{
    static const char *const names[] = {"nothing",   "refuse",   "unsupported", "next",       "back",
                                        "playset",   "show",     "show_file",   "show_url",   "background",
                                        "pause",     "brightness", "rotation"};
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "?";
}
