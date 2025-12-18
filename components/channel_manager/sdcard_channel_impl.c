#include "sdcard_channel_impl.h"
#include "playlist_manager.h"
#include "play_navigator.h"
#include "channel_settings.h"
#include "config_store.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>

static const char *TAG = "sdcard_channel_impl";

typedef enum {
    SDCARD_ENTRY_KIND_ARTWORK = 0,
    SDCARD_ENTRY_KIND_PLAYLIST = 1,
} sdcard_entry_kind_t;

typedef struct {
    sdcard_entry_kind_t kind;
    int32_t post_id;                // Stable across boots (hash of filename)
    time_t created_at;

    // Common
    char *name;                     // filename
    char *filepath;                 // full path

    // Artwork kind
    asset_type_t type;

    // Playlist kind
    uint32_t playlist_total_artworks;
    uint32_t playlist_dwell_time_ms;
} sdcard_entry_t;

/**
 * @brief Internal SD card channel state
 */
typedef struct {
    struct channel_s base;      // Base channel (must be first)

    // Configuration
    char *animations_dir;       // Directory to scan

    // Loaded posts (artwork posts + playlist posts)
    sdcard_entry_t *entries;
    size_t entry_count;

    // Playback (playlist-aware)
    play_navigator_t navigator;
    bool navigator_ready;

    uint32_t channel_dwell_override_ms;
} sdcard_channel_t;

// Forward declarations of interface functions
static esp_err_t sdcard_impl_load(channel_handle_t channel);
static void sdcard_impl_unload(channel_handle_t channel);
static esp_err_t sdcard_impl_start_playback(channel_handle_t channel,
                                            channel_order_mode_t order_mode,
                                            const channel_filter_config_t *filter);
static esp_err_t sdcard_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t sdcard_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t sdcard_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t sdcard_impl_request_reshuffle(channel_handle_t channel);
static esp_err_t sdcard_impl_request_refresh(channel_handle_t channel);
static esp_err_t sdcard_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats);
static void sdcard_impl_destroy(channel_handle_t channel);
static size_t sdcard_impl_get_post_count(channel_handle_t channel);
static esp_err_t sdcard_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post);
static void *sdcard_impl_get_navigator(channel_handle_t channel);

// Virtual function table
static const channel_ops_t s_sdcard_ops = {
    .load = sdcard_impl_load,
    .unload = sdcard_impl_unload,
    .start_playback = sdcard_impl_start_playback,
    .next_item = sdcard_impl_next_item,
    .prev_item = sdcard_impl_prev_item,
    .current_item = sdcard_impl_current_item,
    .request_reshuffle = sdcard_impl_request_reshuffle,
    .request_refresh = sdcard_impl_request_refresh,
    .get_stats = sdcard_impl_get_stats,
    .destroy = sdcard_impl_destroy,
    .get_post_count = sdcard_impl_get_post_count,
    .get_post = sdcard_impl_get_post,
    .get_navigator = sdcard_impl_get_navigator,
};

static uint32_t hash_string_djb2(const char *s)
{
    uint32_t hash = 5381u;
    unsigned char c;
    while (s && (c = (unsigned char)*s++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }
    return hash;
}

static bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    return (stat(path, &st) == 0);
}

static asset_type_t asset_type_from_name(const char *name, bool *out_ok)
{
    if (out_ok) *out_ok = false;
    if (!name) return ASSET_TYPE_WEBP;

    size_t len = strlen(name);
    if (len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) {
        if (out_ok) *out_ok = true;
        return ASSET_TYPE_WEBP;
    }
    if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) {
        if (out_ok) *out_ok = true;
        return ASSET_TYPE_JPEG;
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) {
        if (out_ok) *out_ok = true;
        return ASSET_TYPE_GIF;
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) {
        if (out_ok) *out_ok = true;
        return ASSET_TYPE_PNG;
    }
    if (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) {
        if (out_ok) *out_ok = true;
        return ASSET_TYPE_JPEG;
    }

    return ASSET_TYPE_WEBP;
}

static uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                           uint32_t channel_override_ms,
                                           uint32_t playlist_or_artwork_ms)
{
    uint32_t eff = playlist_or_artwork_ms;
    if (channel_override_ms != 0) eff = channel_override_ms;
    if (global_override_ms != 0) eff = global_override_ms;
    if (eff == 0) eff = 30000;
    return eff;
}

static esp_err_t read_file_to_buf(const char *path, char **out_buf, size_t *out_len)
{
    if (!path || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    *out_buf = buf;
    *out_len = rd;
    return ESP_OK;
}

static bool parse_sd_playlist_file(const char *playlist_path,
                                  const char *animations_dir,
                                  int32_t playlist_post_id,
                                  uint32_t *out_total_artworks,
                                  uint32_t *out_playlist_dwell_ms)
{
    if (out_total_artworks) *out_total_artworks = 0;
    if (out_playlist_dwell_ms) *out_playlist_dwell_ms = 0;

    char *buf = NULL;
    size_t len = 0;
    if (read_file_to_buf(playlist_path, &buf, &len) != ESP_OK) {
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return false;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "artworks");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return false;
    }

    uint32_t playlist_dwell_ms = 0;
    cJSON *dwell = cJSON_GetObjectItem(root, "dwell_time_ms");
    if (dwell && cJSON_IsNumber(dwell)) {
        double v = cJSON_GetNumberValue(dwell);
        if (v >= 0 && v <= 100000000) {
            playlist_dwell_ms = (uint32_t)v;
        }
    }

    int total = cJSON_GetArraySize(arr);
    if (total <= 0) {
        cJSON_Delete(root);
        return false;
    }

    playlist_metadata_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.post_id = playlist_post_id;
    pl.metadata_modified_at = 0;
    pl.total_artworks = total;
    pl.loaded_artworks = total;
    pl.available_artworks = 0;
    pl.dwell_time_ms = playlist_dwell_ms;

    pl.artworks = (artwork_ref_t *)calloc((size_t)total, sizeof(artwork_ref_t));
    if (!pl.artworks) {
        cJSON_Delete(root);
        return false;
    }

    int downloaded_count = 0;
    bool ok = true;

    for (int i = 0; i < total; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        const char *file = NULL;
        uint32_t item_dwell_ms = 0;

        if (cJSON_IsString(it)) {
            file = cJSON_GetStringValue(it);
        } else if (cJSON_IsObject(it)) {
            cJSON *f = cJSON_GetObjectItem(it, "file");
            if (cJSON_IsString(f)) {
                file = cJSON_GetStringValue(f);
            }
            cJSON *dt = cJSON_GetObjectItem(it, "dwell_time_ms");
            if (dt && cJSON_IsNumber(dt)) {
                double v = cJSON_GetNumberValue(dt);
                if (v >= 0 && v <= 100000000) {
                    item_dwell_ms = (uint32_t)v;
                }
            }
        }

        if (!file || !*file) {
            ok = false;
            break;
        }

        char full[512];
        int n = snprintf(full, sizeof(full), "%s/%s", animations_dir, file);
        if (n < 0 || n >= (int)sizeof(full)) {
            ok = false;
            break;
        }

        bool type_ok = false;
        asset_type_t type = asset_type_from_name(file, &type_ok);
        if (!type_ok) {
            ok = false;
            break;
        }

        artwork_ref_t *a = &pl.artworks[i];
        memset(a, 0, sizeof(*a));
        a->post_id = (int32_t)hash_string_djb2(file);
        a->type = type;
        a->dwell_time_ms = item_dwell_ms;
        a->downloaded = file_exists(full);
        if (a->downloaded) downloaded_count++;

        strlcpy(a->filepath, full, sizeof(a->filepath));
        strlcpy(a->storage_key, file, sizeof(a->storage_key));
        a->art_url[0] = '\0';
    }

    if (ok) {
        pl.available_artworks = downloaded_count;
        if (playlist_save_to_disk(&pl) != ESP_OK) {
            ok = false;
        }
    }

    free(pl.artworks);
    cJSON_Delete(root);

    if (!ok) {
        return false;
    }

    if (out_total_artworks) *out_total_artworks = (uint32_t)total;
    if (out_playlist_dwell_ms) *out_playlist_dwell_ms = playlist_dwell_ms;
    return true;
}

static esp_err_t sdcard_impl_load(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;

    if (ch->base.loaded) {
        sdcard_impl_unload(channel);
    }

    const char *dir_path = ch->animations_dir ? ch->animations_dir : ANIMATIONS_DEFAULT_DIR;
    ESP_LOGI(TAG, "Loading SD channel from: %s", dir_path);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", dir_path, errno);
        return ESP_FAIL;
    }

    // Pass 1: count eligible files
    struct dirent *entry;
    size_t count = 0;
    while ((entry = readdir(dir)) != NULL && count < SDCARD_CHANNEL_MAX_POSTS) {
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(entry->d_name);
        bool type_ok = false;
        (void)asset_type_from_name(entry->d_name, &type_ok);
        if (type_ok) {
            count++;
            continue;
        }

        if (len >= 5 && strcasecmp(entry->d_name + len - 5, ".json") == 0) {
            // Ignore metadata sidecars (new convention)
            if (len >= 9 && strcasecmp(entry->d_name + len - 9, "_meta.json") == 0) {
                continue;
            }
            // Candidate playlist
            count++;
        }
    }

    if (count == 0) {
        closedir(dir);
        return ESP_ERR_NOT_FOUND;
    }

    ch->entries = (sdcard_entry_t *)calloc(count, sizeof(sdcard_entry_t));
    if (!ch->entries) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    // Pass 2: load entries, validating playlists
    rewinddir(dir);
    size_t idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < count) {
        if (entry->d_name[0] == '.') continue;

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) continue;

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        size_t name_len = strlen(entry->d_name);
        bool is_json = (name_len >= 5 && strcasecmp(entry->d_name + name_len - 5, ".json") == 0);
        if (is_json && name_len >= 9 && strcasecmp(entry->d_name + name_len - 9, "_meta.json") == 0) {
            // metadata sidecar: ignore
            continue;
        }

        bool type_ok = false;
        asset_type_t type = asset_type_from_name(entry->d_name, &type_ok);

        sdcard_entry_t *e = &ch->entries[idx];
        memset(e, 0, sizeof(*e));
        e->created_at = st.st_mtime;
        e->name = strdup(entry->d_name);
        e->filepath = strdup(full_path);
        if (!e->name || !e->filepath) {
            free(e->name);
            free(e->filepath);
            continue;
        }

        if (type_ok) {
            e->kind = SDCARD_ENTRY_KIND_ARTWORK;
            e->type = type;
            e->post_id = (int32_t)hash_string_djb2(entry->d_name);
            idx++;
            continue;
        }

        if (is_json) {
            int32_t playlist_post_id = (int32_t)hash_string_djb2(entry->d_name);
            uint32_t total_artworks = 0;
            uint32_t playlist_dwell_ms = 0;

            if (!parse_sd_playlist_file(full_path, dir_path, playlist_post_id, &total_artworks, &playlist_dwell_ms)) {
                // Invalid playlist: ignore cleanly
                free(e->name);
                free(e->filepath);
                memset(e, 0, sizeof(*e));
                continue;
            }

            e->kind = SDCARD_ENTRY_KIND_PLAYLIST;
            e->post_id = playlist_post_id;
            e->playlist_total_artworks = total_artworks;
            e->playlist_dwell_time_ms = playlist_dwell_ms;
            idx++;
            continue;
        }

        // Unknown: drop
        free(e->name);
        free(e->filepath);
        memset(e, 0, sizeof(*e));
    }

    closedir(dir);

    ch->entry_count = idx;
    if (ch->entry_count == 0) {
        free(ch->entries);
        ch->entries = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    ch->base.loaded = true;
    ESP_LOGI(TAG, "Loaded %zu SD posts (%s)", ch->entry_count, dir_path);
    return ESP_OK;
}

static void sdcard_impl_unload(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return;

    if (ch->navigator_ready) {
        play_navigator_deinit(&ch->navigator);
        ch->navigator_ready = false;
    }

    if (ch->entries) {
        for (size_t i = 0; i < ch->entry_count; i++) {
            free(ch->entries[i].name);
            free(ch->entries[i].filepath);
        }
        free(ch->entries);
        ch->entries = NULL;
    }

    ch->entry_count = 0;
    ch->base.loaded = false;
}

static size_t sdcard_impl_get_post_count(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->base.loaded) return 0;
    return ch->entry_count;
}

static esp_err_t sdcard_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !out_post) return ESP_ERR_INVALID_ARG;
    if (!ch->base.loaded) return ESP_ERR_INVALID_STATE;
    if (post_index >= ch->entry_count) return ESP_ERR_INVALID_ARG;

    const sdcard_entry_t *e = &ch->entries[post_index];

    memset(out_post, 0, sizeof(*out_post));
    out_post->post_id = e->post_id;
    out_post->created_at = (uint32_t)e->created_at;
    out_post->metadata_modified_at = 0;

    if (e->kind == SDCARD_ENTRY_KIND_PLAYLIST) {
        out_post->kind = CHANNEL_POST_KIND_PLAYLIST;
        out_post->dwell_time_ms = e->playlist_dwell_time_ms;
        out_post->u.playlist.total_artworks = e->playlist_total_artworks;
    } else {
        out_post->kind = CHANNEL_POST_KIND_ARTWORK;
        out_post->dwell_time_ms = 0;
        strlcpy(out_post->u.artwork.filepath, e->filepath, sizeof(out_post->u.artwork.filepath));
        strlcpy(out_post->u.artwork.storage_key, e->name, sizeof(out_post->u.artwork.storage_key));
        out_post->u.artwork.art_url[0] = '\0';
        out_post->u.artwork.type = e->type;
        out_post->u.artwork.width = 0;
        out_post->u.artwork.height = 0;
        out_post->u.artwork.frame_count = 0;
        out_post->u.artwork.has_transparency = false;
        out_post->u.artwork.artwork_modified_at = 0;
    }

    return ESP_OK;
}

static esp_err_t sdcard_impl_start_playback(channel_handle_t channel,
                                            channel_order_mode_t order_mode,
                                            const channel_filter_config_t *filter)
{
    (void)filter;

    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->base.loaded) return ESP_ERR_INVALID_STATE;

    if (ch->navigator_ready) {
        play_navigator_deinit(&ch->navigator);
        ch->navigator_ready = false;
    }

    channel_settings_t settings = {0};
    if (channel_settings_load_for_sdcard(&settings) != ESP_OK) {
        memset(&settings, 0, sizeof(settings));
    }

    // Map channel_order_mode to play_order_mode
    play_order_mode_t play_order = PLAY_ORDER_SERVER;
    switch (order_mode) {
        case CHANNEL_ORDER_ORIGINAL: play_order = PLAY_ORDER_SERVER; break;
        case CHANNEL_ORDER_CREATED:  play_order = PLAY_ORDER_CREATED; break;
        case CHANNEL_ORDER_RANDOM:   play_order = PLAY_ORDER_RANDOM; break;
        default: break;
    }

    if (settings.play_order_present) {
        play_order = (play_order_mode_t)settings.play_order;
    } else {
        play_order = (play_order_mode_t)config_store_get_play_order();
    }

    uint32_t pe = settings.pe_present ? settings.pe : config_store_get_pe();

    if (settings.channel_dwell_time_present) {
        ch->channel_dwell_override_ms = settings.channel_dwell_time_ms;
    } else {
        ch->channel_dwell_override_ms = 0;
    }

    uint32_t global_seed = config_store_get_global_seed();

    esp_err_t err = play_navigator_init(&ch->navigator, channel, "sdcard", play_order, pe, global_seed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init play navigator: %s", esp_err_to_name(err));
        return err;
    }

    play_navigator_set_channel_dwell_override_ms(&ch->navigator, ch->channel_dwell_override_ms);

    play_navigator_set_randomize_playlist(&ch->navigator,
                                         settings.randomize_playlist_present ? settings.randomize_playlist
                                                                             : config_store_get_randomize_playlist());
    play_navigator_set_live_mode(&ch->navigator,
                                 settings.live_mode_present ? settings.live_mode
                                                           : config_store_get_live_mode());

    ch->base.current_order = order_mode;
    ch->navigator_ready = true;
    ESP_LOGI(TAG, "Started SD playback (navigator): posts=%zu order=%d pe=%lu", ch->entry_count, order_mode,
             (unsigned long)pe);
    return ESP_OK;
}

static esp_err_t fill_item_from_artwork(sdcard_channel_t *ch, const artwork_ref_t *art, channel_item_ref_t *out_item)
{
    if (!ch || !art || !out_item) return ESP_ERR_INVALID_ARG;

    memset(out_item, 0, sizeof(*out_item));
    strlcpy(out_item->filepath, art->filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art->storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;

    switch (art->type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }

    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art->dwell_time_ms);
    return ESP_OK;
}

static esp_err_t sdcard_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->navigator_ready) return ESP_ERR_INVALID_STATE;

    artwork_ref_t art;
    esp_err_t err = play_navigator_current(&ch->navigator, &art);
    if (err != ESP_OK) return err;
    return fill_item_from_artwork(ch, &art, out_item);
}

static esp_err_t sdcard_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->navigator_ready) return ESP_ERR_INVALID_STATE;

    artwork_ref_t art;
    esp_err_t err = play_navigator_next(&ch->navigator, &art);
    if (err != ESP_OK) return err;
    return fill_item_from_artwork(ch, &art, out_item);
}

static esp_err_t sdcard_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->navigator_ready) return ESP_ERR_INVALID_STATE;

    artwork_ref_t art;
    esp_err_t err = play_navigator_prev(&ch->navigator, &art);
    if (err != ESP_OK) return err;
    return fill_item_from_artwork(ch, &art, out_item);
}

static esp_err_t sdcard_impl_request_reshuffle(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->navigator_ready) return ESP_ERR_INVALID_STATE;
    return play_navigator_request_reshuffle(&ch->navigator);
}

static esp_err_t sdcard_impl_request_refresh(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;

    channel_order_mode_t order = ch->base.current_order;
    channel_filter_config_t filter = ch->base.current_filter;

    sdcard_impl_unload(channel);
    esp_err_t err = sdcard_impl_load(channel);
    if (err != ESP_OK) {
        return err;
    }

    return sdcard_impl_start_playback(channel, order, &filter);
}

static esp_err_t sdcard_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !out_stats) return ESP_ERR_INVALID_ARG;

    out_stats->total_items = ch->entry_count;
    out_stats->filtered_items = ch->entry_count;

    uint32_t p = 0, q = 0;
    if (ch->navigator_ready) {
        play_navigator_get_position(&ch->navigator, &p, &q);
        out_stats->current_position = p;
    } else {
        out_stats->current_position = 0;
    }

    return ESP_OK;
}

static void sdcard_impl_destroy(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return;

    sdcard_impl_unload(channel);
    free(ch->animations_dir);
    free(ch->base.name);
    free(ch);

    ESP_LOGI(TAG, "Channel destroyed");
}

static void *sdcard_impl_get_navigator(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return NULL;
    return ch->navigator_ready ? (void *)&ch->navigator : NULL;
}

// Public creation function

channel_handle_t sdcard_channel_create(const char *name, const char *animations_dir)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)calloc(1, sizeof(sdcard_channel_t));
    if (!ch) {
        ESP_LOGE(TAG, "Failed to allocate channel");
        return NULL;
    }

    ch->base.ops = &s_sdcard_ops;
    ch->base.name = name ? strdup(name) : strdup("SD Card");
    ch->animations_dir = animations_dir ? strdup(animations_dir) : NULL;
    ch->base.current_order = CHANNEL_ORDER_ORIGINAL;

    if (!ch->base.name) {
        free(ch);
        return NULL;
    }

    ESP_LOGI(TAG, "Created SD card channel: %s", ch->base.name);
    return (channel_handle_t)ch;
}
