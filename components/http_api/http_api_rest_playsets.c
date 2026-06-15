// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_api_rest_playsets.c
 * @brief Playset execution and CRUD REST handlers
 *
 * Contains handlers for:
 * - POST /playset/{name} - Load and execute a named playset
 * - GET /playsets/active - Get active playset name
 * - GET /playsets - List all saved playsets
 * - GET /playsets/{name} - Read a playset (optionally activate)
 * - POST /playsets/{name} - Create/update a playset
 * - DELETE /playsets/{name} - Delete a playset
 */

#include "http_api_internal.h"
#include "makapix_api.h"
#include "makapix_channel_utils.h"
#include "makapix_mqtt.h"
#include "makapix_store.h"
#include "play_scheduler.h"
#include "playset_store.h"
#include "playset_json.h"
#include "p3a_state.h"
#include "p3a_current_post.h"
#include "giphy.h"
#include "art_institution.h"
#include "config_store.h"
#include "psram_alloc.h"
#include "slave_ota.h"

// ---------- Playset Name Validation ----------

static bool is_valid_playset_name(const char *name, size_t len)
{
    if (!name || len == 0 || len > PLAYSET_MAX_NAME_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)name[i] < 0x20) return false;
    }
    return true;
}

// ---------- FNV-1a (used for the /playsets/active weak ETag) ----------
//
// Just needs to flip when any tracked field changes. No cryptographic
// strength required.
static inline uint32_t fnv_u8(uint32_t h, uint8_t b) {
    return (h ^ b) * 0x01000193u;
}
static uint32_t fnv_u32(uint32_t h, uint32_t v) {
    h = fnv_u8(h, (uint8_t)(v & 0xff));
    h = fnv_u8(h, (uint8_t)((v >> 8) & 0xff));
    h = fnv_u8(h, (uint8_t)((v >> 16) & 0xff));
    h = fnv_u8(h, (uint8_t)((v >> 24) & 0xff));
    return h;
}
static uint32_t fnv_str(uint32_t h, const char *s) {
    if (!s) return fnv_u8(h, 0xff);
    while (*s) h = fnv_u8(h, (uint8_t)*s++);
    return fnv_u8(h, 0);
}

// ---------- Current Artwork Helper ----------

static const char *asset_type_ext(asset_type_t t) {
    switch (t) {
        case ASSET_TYPE_WEBP: return ".webp";
        case ASSET_TYPE_GIF:  return ".gif";
        case ASSET_TYPE_PNG:  return ".png";
        case ASSET_TYPE_JPEG: return ".jpg";
        default:              return ".webp";
    }
}

/**
 * Build a "current_artwork" cJSON object from the play scheduler state.
 * Returns NULL if no artwork is playing or URL cannot be built.
 * Caller must cJSON_Delete the result.
 */
cJSON *build_current_artwork_json(void)
{
    ps_artwork_t artwork;
    if (play_scheduler_current(&artwork) != ESP_OK) {
        return NULL;
    }

    char url[300] = "";

    switch (artwork.channel_type) {
        case PS_CHANNEL_TYPE_GIPHY:
            snprintf(url, sizeof(url),
                     "https://i.giphy.com/media/%s/giphy.webp",
                     artwork.storage_key);
            break;

        case PS_CHANNEL_TYPE_NAMED:
        case PS_CHANNEL_TYPE_USER:
        case PS_CHANNEL_TYPE_REACTIONS:
        case PS_CHANNEL_TYPE_HASHTAG:
            (void)makapix_build_remote_url(artwork.storage_key,
                                           asset_type_ext(artwork.type),
                                           url, sizeof(url));
            break;

        case PS_CHANNEL_TYPE_ARTWORK: {
            // Single-source ephemeral playback (Makapix show_artwork or local
            // file). For Makapix, build the same vault URL used by named
            // channels; for local files (storage_key empty) leave url empty.
            if (artwork.storage_key[0] != '\0') {
                (void)makapix_build_remote_url(artwork.storage_key,
                                               asset_type_ext(artwork.type),
                                               url, sizeof(url));
            }
            break;
        }

        case PS_CHANNEL_TYPE_INSTITUTION: {
            // The browser fetches the IIIF URL directly — same origin-URL
            // pattern Giphy and Makapix use, so the device doesn't proxy
            // image bytes. channel_spec_name is "{museum_id}:{axis}";
            // storage_key holds the iiif_key (stamped at pick time, see
            // play_scheduler_pick.c). Extension is 3 (jpg) for AIC and
            // for every museum shipped today.
            char museum_id[16] = {0};
            char axis_unused[32] = {0};
            if (artwork.storage_key[0] != '\0' &&
                art_institution_parse_spec(artwork.channel_spec_name,
                                           museum_id, sizeof(museum_id),
                                           axis_unused, sizeof(axis_unused)) == ESP_OK) {
                institution_channel_entry_t e = {0};
                e.extension = 3;
                strlcpy(e.iiif_key, artwork.storage_key, sizeof(e.iiif_key));
                art_institution_build_iiif_url(museum_id, &e, 720, url, sizeof(url));
            }
            break;
        }

        case PS_CHANNEL_TYPE_PINNED: {
            // Pinned items keep their original post_source; storage_key holds
            // the source's native identifier (Makapix UUID, Giphy id, IIIF
            // key), so we rebuild the same origin URL each native source uses.
            // For institution pins the museum id is stashed in
            // channel_spec_name as "{museum}:pin" by play_scheduler_pick.
            if (artwork.storage_key[0] == '\0') break;
            switch (artwork.post_source) {
                case POST_SOURCE_GIPHY:
                    snprintf(url, sizeof(url),
                             "https://i.giphy.com/media/%s/giphy.webp",
                             artwork.storage_key);
                    break;
                case POST_SOURCE_MAKAPIX:
                    (void)makapix_build_remote_url(artwork.storage_key,
                                                   asset_type_ext(artwork.type),
                                                   url, sizeof(url));
                    break;
                case POST_SOURCE_INSTITUTION: {
                    char museum_id[16] = {0};
                    char axis_unused[32] = {0};
                    if (art_institution_parse_spec(artwork.channel_spec_name,
                                                   museum_id, sizeof(museum_id),
                                                   axis_unused, sizeof(axis_unused)) == ESP_OK) {
                        institution_channel_entry_t e = {0};
                        e.extension = (uint8_t)artwork.type;
                        strlcpy(e.iiif_key, artwork.storage_key, sizeof(e.iiif_key));
                        art_institution_build_iiif_url(museum_id, &e, 720, url, sizeof(url));
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }

        case PS_CHANNEL_TYPE_SDCARD:
        default:
            break;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "channel_type",
                            playset_channel_type_str(artwork.channel_type));

    // Human-readable channel label for the artwork-info panel. Resolves the
    // active channel's stored display_name (matching what the now-playing card
    // shows, including editor-composed institution labels), falling back to a
    // name derived from the artwork's stamped provenance when the channel is no
    // longer in the active playset.
    char channel_name[65];
    play_scheduler_get_channel_display_name(&artwork, channel_name, sizeof(channel_name));
    cJSON_AddStringToObject(obj, "channel_name", channel_name);

    // Fields driven by p3a_current_post (the actual on-screen post, not the
    // next one to be picked). We emit:
    //   - `source`     : string label for the underlying post source. The
    //                    web UI uses this to gate the reaction button
    //                    independently of channel_type (which collapses to
    //                    "pinned" for pinned items and would otherwise hide
    //                    the original makapix/giphy/institution origin).
    //   - `post_id`    : present for Makapix AND Institution sources. The
    //                    /action/pin endpoint accepts post_id for both.
    //   - `giphy_id`   : present for Giphy.
    //   - `reaction_submitted` : present only for Makapix (the only source
    //                    that supports the 👍 reaction-submit semantic).
    int source = p3a_current_post_get_source();
    const char *source_label = "none";
    switch (source) {
        case POST_SOURCE_MAKAPIX:     source_label = "makapix";     break;
        case POST_SOURCE_GIPHY:       source_label = "giphy";       break;
        case POST_SOURCE_SDCARD:      source_label = "sdcard";      break;
        case POST_SOURCE_INSTITUTION: source_label = "institution"; break;
        default:                      source_label = "none";        break;
    }
    cJSON_AddStringToObject(obj, "source", source_label);

    if (source == POST_SOURCE_MAKAPIX) {
        int32_t post_id = p3a_current_post_get_id();
        if (post_id > 0) {
            cJSON_AddNumberToObject(obj, "post_id", (double)post_id);
            cJSON_AddBoolToObject(obj, "reaction_submitted",
                                  p3a_current_post_get_reaction_submitted());
        }
        // storage_key drives the browser's info-fetch against
        // https://makapix.club/api/player/post/{storage_key}. Already populated for
        // Makapix and pinned-Makapix artwork (used at the top of this function
        // to build the vault URL).
        if (artwork.storage_key[0] != '\0') {
            cJSON_AddStringToObject(obj, "storage_key", artwork.storage_key);
        }
    } else if (source == POST_SOURCE_GIPHY) {
        char giphy_id[24];
        p3a_current_post_get_giphy_id(giphy_id, sizeof(giphy_id));
        if (giphy_id[0]) {
            cJSON_AddStringToObject(obj, "giphy_id", giphy_id);
        }
    } else if (source == POST_SOURCE_INSTITUTION) {
        int32_t post_id = p3a_current_post_get_id();
        if (post_id > 0) {
            // post_id only — museums have no reaction-submit counterpart on
            // device, so `reaction_submitted` is omitted. The web UI keys on
            // `source` to gate the reaction button.
            cJSON_AddNumberToObject(obj, "post_id", (double)post_id);
        }
        // museum_id + iiif_key let the browser route the title-fetch to the
        // right per-museum adapter in webui/museum/*.js. channel_spec_name
        // is "{museum}:{axis}" for INSTITUTION channels and "{museum}:pin"
        // for pinned-institution artwork; in both cases the museum prefix is
        // what we want.
        char museum_id[16] = {0};
        char axis_unused[32] = {0};
        if (artwork.storage_key[0] != '\0' &&
            art_institution_parse_spec(artwork.channel_spec_name,
                                       museum_id, sizeof(museum_id),
                                       axis_unused, sizeof(axis_unused)) == ESP_OK) {
            cJSON_AddStringToObject(obj, "museum_id", museum_id);
            cJSON_AddStringToObject(obj, "iiif_key",  artwork.storage_key);
        }
    }

    return obj;
}

// ---------- Playset Execute Handler ----------

/**
 * POST /playset/{name}
 * Resolve a named playset and request an asynchronous switch to it
 *
 * Flow:
 * 1. Check if it's a built-in playset (channel_recent, channel_promoted, channel_sdcard)
 * 2. Else try the local SD library (instant)
 * 3. Else, if MQTT connected, hand the bare name to the switch worker which
 *    fetches from the server (up to ~30 s of retries) off the httpd task
 * 4. Enqueue via play_scheduler_request_switch_* and return immediately
 *
 * The response only confirms acceptance ({"switching":true}). Execution can
 * take seconds for large playsets (per-channel SD cache loads), so completion
 * and errors surface through GET /playsets/active (switching / switch_seq /
 * last_switch_error), which the web UI already polls every 4 s. Boot-restore
 * persistence still happens inside play_scheduler_execute_playset() on the
 * worker task.
 */
esp_err_t h_post_playset(httpd_req_t *req)
{
    // Extract playset name from URI: /playset/{name}
    const char *uri = req->uri;
    const char *prefix = "/playset/";
    size_t prefix_len = strlen(prefix);

    if (strncmp(uri, prefix, prefix_len) != 0) {
        send_json_error(req, 400, "INVALID_PATH", "Invalid playset path");
        return ESP_OK;
    }

    // Copy and URL-decode the name (URI contains percent-encoded characters)
    char name[PLAYSET_MAX_NAME_LEN + 1];
    strlcpy(name, uri + prefix_len, sizeof(name));
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json_error(req, 400, "INVALID_NAME", "Invalid playset name");
        return ESP_OK;
    }

    // Screen-affecting switches are dropped during the slave OTA, matching
    // the /action/swap_to gate and the MQTT command path.
    if (slave_ota_is_in_progress()) {
        send_json_error(req, 503, "OTA_IN_PROGRESS", "OTA in progress");
        return ESP_OK;
    }

    ps_playset_t *playset = psram_calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        send_json_oom(req);
        return ESP_OK;
    }

    esp_err_t err;
    bool from_cache = false;
    bool is_builtin = false;
    bool by_name = false;

    // 1. Check built-in playsets (no I/O needed)
    err = ps_create_channel_playset(name, playset);
    if (err == ESP_OK) {
        is_builtin = true;
        ESP_LOGI("http_api", "Using built-in playset: %s", name);
    } else {
        // 2. Try local cache first (a single small SD read)
        err = playset_store_load(name, playset);
        if (err == ESP_OK) {
            from_cache = true;
        } else if (makapix_mqtt_is_connected()) {
            // 3. Not cached locally -- the switch worker fetches it from the
            // server so the MQTT wait doesn't block the httpd task. A
            // nonexistent playset therefore reports PLAYSET_NOT_FOUND via
            // the status poll instead of a synchronous 404.
            by_name = true;
        } else {
            free(playset);
            send_json_error(req, 503, "NOT_CONNECTED", "Not connected and no cached playset");
            return ESP_OK;
        }
    }

    // Hand off to the async switch worker. channel_count is captured before
    // the enqueue: on success the worker owns `playset` and it must not be
    // touched afterwards (unknown for by-name requests).
    int channel_count = by_name ? -1 : (int)playset->channel_count;
    esp_err_t enq;
    if (by_name) {
        free(playset);
        enq = play_scheduler_request_switch_by_name(name);
    } else {
        enq = play_scheduler_request_switch_by_value(playset, name);
        if (enq != ESP_OK) {
            free(playset);  // ownership retained on enqueue failure
        }
    }
    if (enq != ESP_OK) {
        send_json_error(req, 503, "SWITCH_UNAVAILABLE", "Switch worker unavailable");
        return ESP_OK;
    }

    // Build the acceptance response. The pre-switch total_cached /
    // total_entries / pick_mode fields are gone — the web UI hydrates the
    // now-playing card from the status poll once the switch lands.
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "switching", true);
    cJSON_AddStringToObject(root, "playset", name);
    if (channel_count >= 0) {
        cJSON_AddNumberToObject(root, "channel_count", (double)channel_count);
    }
    cJSON_AddBoolToObject(root, "from_cache", from_cache);
    cJSON_AddBoolToObject(root, "builtin", is_builtin);

    send_json_root(req, 200, root);
    return ESP_OK;
}

// ---------- Playset CRUD Handlers ----------

/**
 * GET /playsets/active
 * Returns the currently active playset name.
 * This is the playset-centric replacement for GET /channel's playset field.
 * (GET /channel is deprecated and will be removed in a future version.)
 */
esp_err_t h_get_active_playset(httpd_req_t *req)
{
    // Gather every value that ends up in the response (and in the ETag
    // digest) once, up front. The web UI polls this endpoint every 4s —
    // most polls find nothing changed, so the 304 short-circuit needs the
    // input values without paying for cJSON allocation first.
    //
    // Async-switch short-circuit: while the switch worker is executing a
    // playset it holds the scheduler state mutex for seconds (per-channel SD
    // cache loads), and every play_scheduler_* getter below would block on
    // it — freezing exactly the poll that should stay responsive during a
    // switch. The switch status getter never touches that mutex; when it
    // reports switching, skip the scheduler-derived gathers entirely (their
    // payload sections are omitted and the web UI keeps showing its
    // last-known state under a "switching" indicator).
    ps_switch_status_t sw;
    play_scheduler_get_switch_status(&sw);

    ps_playset_t *active = calloc(1, sizeof(ps_playset_t));
    bool has_active = (!sw.switching && active &&
                       play_scheduler_get_active_playset(active) == ESP_OK);

    bool registered = makapix_store_has_player_key();
    // The active playset has non-open Makapix channels but the device has no
    // usable registration, so those channels can never refresh. Computed from
    // the already-loaded `active` (guarded by has_active, false during a switch,
    // matching the other scheduler-derived fields). Drives the home-page banner.
    bool makapix_reg_required = has_active &&
        play_scheduler_playset_needs_registration(active);
    bool cooldown_active = giphy_is_rate_limited();
    uint32_t giphy_cd_sec = cooldown_active ? giphy_cooldown_remaining_sec() : 0;
    bool giphy_auth_bad = giphy_is_auth_invalid();
    bool giphy_no_key_set = giphy_is_no_key();
    uint32_t giphy_refresh_int = config_store_get_giphy_refresh_interval();
    uint32_t refresh_int = config_store_get_refresh_interval_sec();
    bool refresh_allow_override = config_store_get_refresh_allow_override();

    ps_stats_t ps_stats;
    bool has_stats = (!sw.switching && play_scheduler_get_stats(&ps_stats) == ESP_OK);

    ps_channel_detail_t *ch_details = calloc(PS_MAX_CHANNELS, sizeof(ps_channel_detail_t));
    size_t ch_count = 0;
    if (ch_details && has_stats) {
        if (play_scheduler_get_channel_details(ch_details, PS_MAX_CHANNELS, &ch_count) != ESP_OK) {
            ch_count = 0;
        }
    }

    // The current-post gathers don't lock scheduler state, but they pair with
    // current_artwork (omitted while switching) — gate them with the same
    // flag so the ETag never depends on fields the payload doesn't carry.
    ps_artwork_t cur_aw;
    bool has_cur_aw = (!sw.switching && play_scheduler_current(&cur_aw) == ESP_OK);
    int cur_post_source = 0;
    int32_t cur_post_id = 0;
    bool cur_reaction_submitted = false;
    char cur_giphy_id[24] = {0};
    if (!sw.switching) {
        cur_post_source = p3a_current_post_get_source();
        cur_post_id = p3a_current_post_get_id();
        cur_reaction_submitted = p3a_current_post_get_reaction_submitted();
        p3a_current_post_get_giphy_id(cur_giphy_id, sizeof(cur_giphy_id));
    }

    // Per-museum rate-limit cooldowns. Folded into this endpoint so the
    // web UI doesn't need a separate /api/museum/rate-limits poll — the web
    // UI uses each value only as a boolean (badge show/hide), and the same
    // ETag mechanism that covers the giphy cooldown extends to these
    // (boolean hashed, not seconds). The standalone endpoint is kept for
    // webui/museum/browse.js which does a one-shot lookup after a 429.
    uint32_t museum_remaining[ART_INSTITUTION_MUSEUM_COUNT];
    // BYOK museums (HAM, SI) whose user-supplied key is currently unset:
    // refresh can never run, so the web UI flags the channel and offers a
    // jump to Settings. Boolean (not playset-gated) — the web UI gates it
    // per channel row, same as the cooldowns above.
    bool museum_key_missing[ART_INSTITUTION_MUSEUM_COUNT];
    for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
        museum_remaining[i] = art_institution_rate_limit_remaining(
            ART_INSTITUTION_MUSEUMS[i].id);
        museum_key_missing[i] = art_institution_api_key_missing(
            ART_INSTITUTION_MUSEUMS[i].id);
    }

    // ----- Weak ETag over stable fields -----
    //
    // The per-second `giphy_cd_sec` countdown is intentionally NOT hashed.
    // We feed the derived `cooldown_active` boolean instead. The web UI
    // uses the cooldown value only as a boolean (banner show/hide), so a
    // 304 streak can last the entire cooldown window — flipping only when
    // the cooldown engages or expires. Hashing the seconds would defeat
    // the whole optimisation: it changes on every single poll.
    uint32_t h = 0x811c9dc5u;
    h = fnv_u8(h, registered ? 1 : 0);
    // Boolean, so 304-friendly (flips only when registration state or the active
    // playset's channel mix changes), same rationale as the giphy flags below.
    h = fnv_u8(h, makapix_reg_required ? 1 : 0);
    h = fnv_u8(h, cooldown_active ? 1 : 0);
    // Auth-invalid latch and no-key flag are already booleans — they flip
    // once per episode, so they're 304-friendly by nature (same rationale
    // as cooldown_active above).
    h = fnv_u8(h, giphy_auth_bad ? 1 : 0);
    h = fnv_u8(h, giphy_no_key_set ? 1 : 0);
    h = fnv_u32(h, giphy_refresh_int);
    h = fnv_u32(h, refresh_int);
    h = fnv_u8(h, refresh_allow_override ? 1 : 0);
    // Async switch progress. switch_seq bumps once per completed attempt, so
    // the 304 streak breaks exactly when a switch starts (switching flips
    // true) and when it lands (seq advances, switching flips back). During
    // the switch itself every field here is stable — polls 304 right through
    // the worker's multi-second execute.
    h = fnv_u8(h, sw.switching ? 1 : 0);
    h = fnv_str(h, sw.pending_name);
    h = fnv_u32(h, sw.switch_seq);
    h = fnv_str(h, sw.last_error_code);
    if (has_active) {
        h = fnv_str(h, active->name);
    }
    if (has_stats) {
        h = fnv_u32(h, (uint32_t)ps_stats.channel_count);
        h = fnv_u32(h, (uint32_t)ps_stats.total_available);
        h = fnv_u32(h, (uint32_t)ps_stats.total_entries);
        h = fnv_u32(h, (uint32_t)ps_stats.pick_mode);
    }
    for (size_t i = 0; i < ch_count; i++) {
        h = fnv_u32(h, (uint32_t)ch_details[i].type);
        h = fnv_str(h, ch_details[i].display_name);
        h = fnv_str(h, ch_details[i].spec_name);
        h = fnv_str(h, ch_details[i].identifier);
        h = fnv_u32(h, (uint32_t)ch_details[i].available_count);
        h = fnv_u32(h, (uint32_t)ch_details[i].entry_count);
        h = fnv_u8(h, ch_details[i].refreshing ? 1 : 0);
        h = fnv_u32(h, (uint32_t)ch_details[i].last_refresh);
    }
    if (has_cur_aw) {
        h = fnv_u32(h, (uint32_t)cur_aw.channel_type);
        h = fnv_str(h, cur_aw.storage_key);
        h = fnv_str(h, cur_aw.channel_spec_name);
        h = fnv_u32(h, (uint32_t)cur_aw.type);
        h = fnv_u32(h, (uint32_t)cur_aw.post_source);
    }
    h = fnv_u32(h, (uint32_t)cur_post_source);
    h = fnv_u32(h, (uint32_t)cur_post_id);
    h = fnv_u8(h, cur_reaction_submitted ? 1 : 0);
    h = fnv_str(h, cur_giphy_id);
    for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
        // Hash the museum id (defensive against future reorderings) and the
        // boolean derived from remaining_sec — NOT the seconds, same reason
        // as the giphy cooldown above. The key-missing boolean flips once per
        // key save/clear, so it's 304-friendly too.
        h = fnv_str(h, ART_INSTITUTION_MUSEUMS[i].id);
        h = fnv_u8(h, museum_remaining[i] > 0 ? 1 : 0);
        h = fnv_u8(h, museum_key_missing[i] ? 1 : 0);
    }

    char etag[20];
    snprintf(etag, sizeof(etag), "W/\"%08x\"", (unsigned)h);

    // 304 short-circuit. If the client's If-None-Match matches our current
    // digest, the playset/channel/artwork/cooldown-active state is unchanged
    // since the client's last fetch — return an empty body and let them
    // keep the cached payload.
    char inm[32];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_send(req, NULL, 0);
        free(active);
        free(ch_details);
        return ESP_OK;
    }

    // ----- Build the full JSON body -----
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(active);
        free(ch_details);
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *data = cJSON_AddObjectToObject(root, "data");

    /* Emit the structured active playset (channels, types, artwork sub-struct
       for ARTWORK channels). Absent when nothing has been executed yet. The
       WebUI derives pill matching and now-playing labels from this object. */
    if (has_active) {
        cJSON *ap = playset_json_serialize(active);
        if (ap) cJSON_AddItemToObject(data, "active_playset", ap);
    }

    cJSON_AddBoolToObject(data, "registered", registered);

    // The active playset has non-open Makapix channels but the device isn't
    // registered (or its credentials were rejected), so those channels stay
    // empty and never refresh. Drives the home-page "registration needed" banner
    // pointing the user at Settings → Makapix. Same banner mechanism as the
    // giphy flags below.
    cJSON_AddBoolToObject(data, "makapix_registration_required", makapix_reg_required);

    cJSON_AddNumberToObject(data, "giphy_cooldown_remaining_sec", (double)giphy_cd_sec);

    // Giphy rejected the configured API key (HTTP 401/403); refreshes are
    // parked until a key save or the hourly reprobe. Drives the home-page
    // banner pointing the user at Settings.
    cJSON_AddBoolToObject(data, "giphy_auth_invalid", giphy_auth_bad);

    // The playset has Giphy channels but no API key is configured; refreshes
    // are parked until a key save (or the hourly config re-read). Same
    // banner mechanism, softer tone — it's an unconfigured state, not an
    // error.
    cJSON_AddBoolToObject(data, "giphy_no_key", giphy_no_key_set);

    // Refresh intervals so the frontend can derive due-for-refresh from
    // ch.last_refresh without a backend round trip. Cached statics — cheap.
    cJSON_AddNumberToObject(data, "giphy_refresh_interval_sec", (double)giphy_refresh_int);
    cJSON_AddNumberToObject(data, "refresh_interval_sec", (double)refresh_int);

    // Refresh override (one-time bypass): when true the dispatcher refreshes
    // every channel regardless of its last_refresh, so the frontend's
    // freshness-gated pulse animation must also bypass its due-check or the
    // user sees real refreshes happening with no visual feedback. The flag
    // auto-resets on the play scheduler side after the sweep completes.
    cJSON_AddBoolToObject(data, "refresh_allow_override", refresh_allow_override);

    // Async switch progress (always emitted). While `switching` is true the
    // scheduler-derived sections (active_playset, playset_info,
    // current_artwork) are omitted — the web UI keeps its last-known display
    // under a pending indicator. `switch_seq` + `last_switch_error` let the
    // poller detect a completed attempt and toast failures (error code is ""
    // on success).
    cJSON_AddBoolToObject(data, "switching", sw.switching);
    cJSON_AddStringToObject(data, "pending_playset", sw.pending_name);
    cJSON_AddNumberToObject(data, "switch_seq", (double)sw.switch_seq);
    cJSON_AddStringToObject(data, "last_switch_error", sw.last_error_code);

    if (has_stats) {
        cJSON *pi = cJSON_AddObjectToObject(data, "playset_info");
        cJSON_AddNumberToObject(pi, "channel_count", (double)ps_stats.channel_count);
        cJSON_AddNumberToObject(pi, "total_cached", (double)ps_stats.total_available);
        cJSON_AddNumberToObject(pi, "total_entries", (double)ps_stats.total_entries);
        cJSON_AddStringToObject(pi, "pick_mode", pick_mode_str(ps_stats.pick_mode));

        if (ch_count > 0) {
            cJSON *ch_arr = cJSON_AddArrayToObject(pi, "channels");
            for (size_t i = 0; i < ch_count; i++) {
                cJSON *ch_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(ch_obj, "display_name", ch_details[i].display_name);
                cJSON_AddStringToObject(ch_obj, "type", playset_channel_type_str(ch_details[i].type));
                cJSON_AddStringToObject(ch_obj, "name", ch_details[i].spec_name);
                cJSON_AddStringToObject(ch_obj, "identifier", ch_details[i].identifier);
                cJSON_AddNumberToObject(ch_obj, "available", (double)ch_details[i].available_count);
                cJSON_AddNumberToObject(ch_obj, "total", (double)ch_details[i].entry_count);
                cJSON_AddBoolToObject(ch_obj, "refreshing", ch_details[i].refreshing);
                cJSON_AddNumberToObject(ch_obj, "last_refresh", (double)ch_details[i].last_refresh);
                cJSON_AddItemToArray(ch_arr, ch_obj);
            }
        }
    }

    // Gated on the switch short-circuit: build_current_artwork_json() calls
    // play_scheduler_current(), which blocks on the scheduler mutex the
    // executing switch is holding.
    cJSON *ca = sw.switching ? NULL : build_current_artwork_json();
    if (ca) {
        cJSON_AddItemToObject(data, "current_artwork", ca);
    }

    // Same shape as /api/museum/rate-limits so the consumer code in
    // index.html can read either source without branching.
    cJSON *mrl = cJSON_AddObjectToObject(data, "museum_rate_limits");
    if (mrl) {
        for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddNumberToObject(o, "remaining_sec", (double)museum_remaining[i]);
            cJSON_AddItemToObject(mrl, ART_INSTITUTION_MUSEUMS[i].id, o);
        }
    }

    // BYOK key state per museum: true ⇒ user must add an API key in Settings
    // before this museum's channels can refresh. Only museums that are BYOK
    // appear (those with an api_key_missing callback); museums shipping a
    // built-in key are omitted so the web UI never flags them.
    cJSON *mkm = cJSON_AddObjectToObject(data, "museum_key_missing");
    if (mkm) {
        for (size_t i = 0; i < ART_INSTITUTION_MUSEUM_COUNT; i++) {
            if (!ART_INSTITUTION_MUSEUMS[i].api_key_missing) continue;
            cJSON_AddBoolToObject(mkm, ART_INSTITUTION_MUSEUMS[i].id,
                                  museum_key_missing[i]);
        }
    }

    free(active);
    free(ch_details);

    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    send_json_root(req, 200, root);
    return ESP_OK;
}

/**
 * GET /playsets
 * List all saved playsets
 */
esp_err_t h_get_playsets(httpd_req_t *req)
{
    playset_list_entry_t *entries = calloc(PLAYSET_MAX_COUNT, sizeof(playset_list_entry_t));
    if (!entries) {
        send_json_oom(req);
        return ESP_OK;
    }

    size_t count = 0;
    esp_err_t err = playset_store_list(entries, PLAYSET_MAX_COUNT, &count);
    if (err != ESP_OK) {
        free(entries);
        send_json_error(req, 500, "LIST_ERROR", "Failed to list playsets");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(entries);
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON *arr = cJSON_AddArrayToObject(data, "playsets");

    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "channel_count", (double)entries[i].channel_count);
        cJSON_AddItemToArray(arr, item);
    }

    free(entries);

    send_json_root(req, 200, root);
    return ESP_OK;
}

/**
 * GET /playsets/{name}[?activate=true]
 * Read a playset; optionally activate it
 */
esp_err_t h_get_playset_by_name(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *prefix = "/playsets/";
    size_t prefix_len = 10; // strlen("/playsets/")

    if (strncmp(uri, prefix, prefix_len) != 0 || uri[prefix_len] == '\0') {
        send_json_error(req, 400, "INVALID_PATH", "Invalid path");
        return ESP_OK;
    }

    const char *name_start = uri + prefix_len;
    const char *qmark = strchr(name_start, '?');
    size_t raw_len = qmark ? (size_t)(qmark - name_start) : strlen(name_start);

    char name[PLAYSET_MAX_NAME_LEN + 1];
    if (raw_len >= sizeof(name)) raw_len = sizeof(name) - 1;
    memcpy(name, name_start, raw_len);
    name[raw_len] = '\0';
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json_error(req, 400, "INVALID_NAME", "Invalid playset name");
        return ESP_OK;
    }

    bool activate = false;
    if (qmark) {
        activate = (strstr(qmark, "activate=true") != NULL);
    }

    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        send_json_oom(req);
        return ESP_OK;
    }

    esp_err_t err = playset_store_load(name, playset);
    if (err == ESP_ERR_NOT_FOUND) {
        free(playset);
        send_json_error(req, 404, "NOT_FOUND", "Playset not found");
        return ESP_OK;
    } else if (err != ESP_OK) {
        free(playset);
        send_json_error(req, 500, "LOAD_ERROR", "Failed to load playset");
        return ESP_OK;
    }

    bool activated = false;
    if (activate && !slave_ota_is_in_progress()) {
        /* Async hand-off: `activated` now means "switch accepted". The worker
           persists the snapshot via execute_playset; completion/errors surface
           through GET /playsets/active. The serialized `playset` below stays
           handler-owned — the worker owns the separate copy. */
        ps_playset_t *copy = psram_calloc(1, sizeof(ps_playset_t));
        if (copy) {
            *copy = *playset;
            if (play_scheduler_request_switch_by_value(copy, name) == ESP_OK) {
                activated = true;
            } else {
                free(copy);
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(playset);
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *data_obj = playset_json_serialize(playset);
    free(playset);

    if (!data_obj) {
        cJSON_Delete(root);
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddItemToObject(root, "data", data_obj);
    cJSON_AddBoolToObject(root, "activated", activated);

    send_json_root(req, 200, root);
    return ESP_OK;
}

/**
 * POST /playsets/{name}
 * Create/update a playset; optionally activate it
 */
esp_err_t h_post_playset_crud(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *prefix = "/playsets/";
    size_t prefix_len = 10;

    if (strncmp(uri, prefix, prefix_len) != 0 || uri[prefix_len] == '\0') {
        send_json_error(req, 400, "INVALID_PATH", "Invalid path");
        return ESP_OK;
    }

    const char *name_start = uri + prefix_len;
    const char *qmark = strchr(name_start, '?');
    size_t raw_len = qmark ? (size_t)(qmark - name_start) : strlen(name_start);

    char name[PLAYSET_MAX_NAME_LEN + 1];
    if (raw_len >= sizeof(name)) raw_len = sizeof(name) - 1;
    memcpy(name, name_start, raw_len);
    name[raw_len] = '\0';
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json_error(req, 400, "INVALID_NAME", "Invalid playset name");
        return ESP_OK;
    }

    // Protected playsets cannot be overwritten via REST API. Currently only
    // "followed_artists" is server-managed; single-artwork and single-local-file
    // playback no longer use reserved name sentinels.
    static const char *protected_playsets[] = { "followed_artists" };
    for (size_t i = 0; i < sizeof(protected_playsets) / sizeof(protected_playsets[0]); i++) {
        if (strcmp(name, protected_playsets[i]) == 0) {
            send_json_error(req, 403, "PROTECTED_PLAYSET", "Cannot overwrite protected playset");
            return ESP_OK;
        }
    }

    if (!ensure_json_content(req)) {
        send_json_error(req, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        return ESP_OK;
    }

    cJSON *root = recv_json_object(req);
    if (!root) return ESP_OK;

    // Extract optional "activate" boolean
    bool activate = false;
    cJSON *activate_item = cJSON_GetObjectItem(root, "activate");
    if (activate_item && cJSON_IsBool(activate_item)) {
        activate = cJSON_IsTrue(activate_item);
    }

    // Extract optional "rename_from" string (must copy before cJSON_Delete)
    char rename_from[PLAYSET_MAX_NAME_LEN + 1] = {0};
    cJSON *rename_item = cJSON_GetObjectItem(root, "rename_from");
    if (rename_item && cJSON_IsString(rename_item) && rename_item->valuestring[0] != '\0') {
        strncpy(rename_from, rename_item->valuestring, PLAYSET_MAX_NAME_LEN);
        rename_from[PLAYSET_MAX_NAME_LEN] = '\0';
    }

    // Parse playset
    ps_playset_t *playset = calloc(1, sizeof(ps_playset_t));
    if (!playset) {
        cJSON_Delete(root);
        send_json_oom(req);
        return ESP_OK;
    }

    esp_err_t parse_err = playset_json_parse(root, playset);
    cJSON_Delete(root);

    if (parse_err != ESP_OK) {
        free(playset);
        send_json_error(req, 400, "INVALID_PLAYSET", "Invalid playset definition");
        return ESP_OK;
    }

    strlcpy(playset->name, name, sizeof(playset->name));
    esp_err_t save_err = playset_store_save(name, playset);
    if (save_err != ESP_OK) {
        free(playset);
        send_json_errorf(req, 500, "SAVE_ERROR", "Failed to save playset: %s",
                         esp_err_to_name(save_err));
        return ESP_OK;
    }

    // Activate if requested. Async hand-off: `activated` now means "switch
    // accepted"; the worker persists the snapshot via execute_playset and
    // completion/errors surface through GET /playsets/active.
    bool activated = false;
    if (activate && !slave_ota_is_in_progress()) {
        ps_playset_t *copy = psram_calloc(1, sizeof(ps_playset_t));
        if (copy) {
            *copy = *playset;
            if (play_scheduler_request_switch_by_value(copy, name) == ESP_OK) {
                activated = true;
            } else {
                free(copy);
            }
        }
    }

    free(playset);

    // Handle rename: delete the old library file if rename_from is set and
    // different from the new name. The in-memory active-playset snapshot keeps
    // the old name in its `name` field — if the renamed playset is currently
    // playing, the pill-bar highlight will only update on the user's next
    // channel switch, which is a fine cost for this corner case.
    bool renamed = false;
    if (rename_from[0] != '\0' && strcmp(rename_from, name) != 0) {
        bool rename_protected = false;
        for (size_t i = 0; i < sizeof(protected_playsets) / sizeof(protected_playsets[0]); i++) {
            if (strcmp(rename_from, protected_playsets[i]) == 0) {
                rename_protected = true;
                break;
            }
        }
        if (!rename_protected) {
            playset_store_delete(rename_from);
            renamed = true;
        }
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        send_json_oom(req);
        return ESP_OK;
    }

    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON *data = cJSON_AddObjectToObject(resp, "data");
    cJSON_AddBoolToObject(data, "saved", true);
    cJSON_AddBoolToObject(data, "activated", activated);
    cJSON_AddBoolToObject(data, "renamed", renamed);

    send_json_root(req, 200, resp);
    return ESP_OK;
}

/**
 * DELETE /playsets/{name}
 * Delete a saved playset
 */
esp_err_t h_delete_playset(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *prefix = "/playsets/";
    size_t prefix_len = 10;

    if (strncmp(uri, prefix, prefix_len) != 0 || uri[prefix_len] == '\0') {
        send_json_error(req, 400, "INVALID_PATH", "Invalid path");
        return ESP_OK;
    }

    const char *name_start = uri + prefix_len;
    const char *qmark = strchr(name_start, '?');
    size_t raw_len = qmark ? (size_t)(qmark - name_start) : strlen(name_start);

    char name[PLAYSET_MAX_NAME_LEN + 1];
    if (raw_len >= sizeof(name)) raw_len = sizeof(name) - 1;
    memcpy(name, name_start, raw_len);
    name[raw_len] = '\0';
    url_decode_in_place(name);

    size_t name_len = strlen(name);
    if (!is_valid_playset_name(name, name_len)) {
        send_json_error(req, 400, "INVALID_NAME", "Invalid playset name");
        return ESP_OK;
    }

    // Protected playsets cannot be deleted
    static const char *protected_playsets[] = { "followed_artists" };
    for (size_t i = 0; i < sizeof(protected_playsets) / sizeof(protected_playsets[0]); i++) {
        if (strcmp(name, protected_playsets[i]) == 0) {
            send_json_error(req, 403, "PROTECTED_PLAYSET", "Cannot delete protected playset");
            return ESP_OK;
        }
    }

    if (!playset_store_exists(name)) {
        send_json_error(req, 404, "NOT_FOUND", "Playset not found");
        return ESP_OK;
    }

    esp_err_t err = playset_store_delete(name);
    if (err != ESP_OK) {
        send_json_error(req, 500, "DELETE_ERROR", "Failed to delete playset");
        return ESP_OK;
    }

    send_json(req, 200, "{\"ok\":true}");
    return ESP_OK;
}
