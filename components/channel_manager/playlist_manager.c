// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "playlist_manager.h"
#include "vault_storage.h"
#include "makapix_api.h"
#include "sd_path.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "playlist_mgr";

typedef struct {
    char storage_key[96];
    char filepath[256];
    uint32_t refcount;
} refcount_entry_t;

// Forward declare for refcount rebuild (definition below)
static esp_err_t load_playlist_json(const char *filepath, playlist_metadata_t **out_playlist);

static bool is_vault_filepath(const char *path)
{
    if (!path) return false;
    return (strstr(path, "/vault/") != NULL);
}

static char *build_meta_path_from_asset_path(const char *filepath)
{
    if (!filepath) return NULL;
    size_t len = strlen(filepath);
    const char *dot = strrchr(filepath, '.');
    const char *slash = strrchr(filepath, '/');
    if (dot && slash && dot < slash) {
        dot = NULL;
    }
    size_t stem_len = dot ? (size_t)(dot - filepath) : len;
    size_t out_len = stem_len + 9 + 1; // "_meta.json"
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;
    memcpy(out, filepath, stem_len);
    strcpy(out + stem_len, "_meta.json");
    return out;
}

static esp_err_t write_refcount_meta(const char *asset_filepath, uint32_t refcount)
{
    if (!asset_filepath) return ESP_ERR_INVALID_ARG;
    if (!is_vault_filepath(asset_filepath)) return ESP_OK;

    struct stat st;
    if (stat(asset_filepath, &st) != 0) return ESP_ERR_NOT_FOUND;

    char *meta_path = build_meta_path_from_asset_path(asset_filepath);
    if (!meta_path) return ESP_ERR_NO_MEM;

    FILE *f = fopen(meta_path, "w");
    if (!f) {
        free(meta_path);
        return ESP_FAIL;
    }

    fprintf(f, "{\"refcount\":%lu,\"size\":%lu}\n", (unsigned long)refcount, (unsigned long)st.st_size);
    fclose(f);
    free(meta_path);
    return ESP_OK;
}

static esp_err_t rebuild_vault_refcounts_from_playlists(void)
{
    char playlists_dir[128];
    if (sd_path_get_playlists(playlists_dir, sizeof(playlists_dir)) != ESP_OK) {
        return ESP_FAIL;
    }

    DIR *dir = opendir(playlists_dir);
    if (!dir) return ESP_OK;

    refcount_entry_t *entries = NULL;
    size_t entry_count = 0;
    size_t entry_cap = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t nlen = strlen(de->d_name);
        if (nlen < 6 || strcasecmp(de->d_name + nlen - 5, ".json") != 0) continue;

        char path[256];
        int w = snprintf(path, sizeof(path), "%s/%s", playlists_dir, de->d_name);
        if (w < 0 || w >= (int)sizeof(path)) continue;

        playlist_metadata_t *pl = NULL;
        if (load_playlist_json(path, &pl) != ESP_OK || !pl) continue;

        for (int32_t i = 0; i < pl->loaded_artworks; i++) {
            const artwork_ref_t *a = &pl->artworks[i];
            if (!is_vault_filepath(a->filepath)) continue;
            if (a->storage_key[0] == '\0') continue;

            // Find or insert by storage_key
            size_t j;
            for (j = 0; j < entry_count; j++) {
                if (strcmp(entries[j].storage_key, a->storage_key) == 0) {
                    entries[j].refcount++;
                    break;
                }
            }
            if (j == entry_count) {
                if (entry_count == entry_cap) {
                    size_t new_cap = entry_cap ? entry_cap * 2 : 32;
                    refcount_entry_t *tmp = (refcount_entry_t *)realloc(entries, new_cap * sizeof(refcount_entry_t));
                    if (!tmp) {
                        playlist_free(pl);
                        free(entries);
                        closedir(dir);
                        return ESP_ERR_NO_MEM;
                    }
                    entries = tmp;
                    entry_cap = new_cap;
                }
                memset(&entries[entry_count], 0, sizeof(entries[entry_count]));
                strlcpy(entries[entry_count].storage_key, a->storage_key, sizeof(entries[entry_count].storage_key));
                strlcpy(entries[entry_count].filepath, a->filepath, sizeof(entries[entry_count].filepath));
                entries[entry_count].refcount = 1;
                entry_count++;
            }
        }

        playlist_free(pl);
    }

    closedir(dir);

    for (size_t i = 0; i < entry_count; i++) {
        (void)write_refcount_meta(entries[i].filepath, entries[i].refcount);
    }

    free(entries);
    return ESP_OK;
}

// Current cached playlist
static playlist_metadata_t *s_current_playlist = NULL;
static int32_t s_current_playlist_id = -1;

// Forward declarations
static esp_err_t parse_artwork_from_json(cJSON *artwork_json, artwork_ref_t *out_artwork);
static esp_err_t parse_playlist_from_json(cJSON *json, playlist_metadata_t **out_playlist);
static esp_err_t save_playlist_json(playlist_metadata_t *playlist, const char *filepath);
static esp_err_t load_playlist_json(const char *filepath, playlist_metadata_t **out_playlist);

// Best-effort ISO8601 UTC parser: "YYYY-MM-DDTHH:MM:SSZ" -> time_t. Returns 0 on failure.
static time_t parse_iso8601_utc(const char *s)
{
    if (!s || !*s) return 0;
    int year, month, day, hour, min, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec) != 6) {
        return 0;
    }
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    return mktime(&tm);
}

static asset_type_t asset_type_from_url(const char *url)
{
    if (!url) return ASSET_TYPE_WEBP;
    const char *ext = strrchr(url, '.');
    if (!ext) return ASSET_TYPE_WEBP;
    if (strcasecmp(ext, ".webp") == 0) return ASSET_TYPE_WEBP;
    if (strcasecmp(ext, ".gif") == 0) return ASSET_TYPE_GIF;
    if (strcasecmp(ext, ".png") == 0) return ASSET_TYPE_PNG;
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return ASSET_TYPE_JPEG;
    return ASSET_TYPE_WEBP;
}

static esp_err_t storage_key_sha256(const char *storage_key, uint8_t out_sha256[32])
{
    if (!storage_key || !out_sha256) return ESP_ERR_INVALID_ARG;
    int ret = mbedtls_sha256((const unsigned char *)storage_key, strlen(storage_key), out_sha256, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA256 failed (ret=%d)", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Build vault path based on storage_key UUID (matches makapix_artwork_download sharding).
static void build_vault_path_from_storage_key_uuid(const char *storage_key, const char *art_url, char *out, size_t out_len)
{
    if (!storage_key || !out || out_len == 0) return;
    
    char vault_base[128];
    if (sd_path_get_vault(vault_base, sizeof(vault_base)) != ESP_OK) {
        snprintf(out, out_len, "%s/vault/%s.webp", SD_PATH_DEFAULT_ROOT, storage_key);
        return;
    }
    
    uint8_t sha256[32];
    if (storage_key_sha256(storage_key, sha256) != ESP_OK) {
        // Best-effort fallback (no sharding)
        snprintf(out, out_len, "%s/%s.webp", vault_base, storage_key);
        return;
    }
    char dir1[3], dir2[3], dir3[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)sha256[0]);
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)sha256[1]);
    snprintf(dir3, sizeof(dir3), "%02x", (unsigned int)sha256[2]);

    const char *ext = ".webp";
    if (art_url) {
        const char *dot = strrchr(art_url, '.');
        if (dot) {
            if (strcasecmp(dot, ".webp") == 0) ext = ".webp";
            else if (strcasecmp(dot, ".gif") == 0) ext = ".gif";
            else if (strcasecmp(dot, ".png") == 0) ext = ".png";
            else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) ext = ".jpg";
        }
    }

    snprintf(out, out_len, "%s/%s/%s/%s/%s%s", vault_base, dir1, dir2, dir3, storage_key, ext);
}

esp_err_t playlist_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing playlist manager");
    
    char playlists_dir[128];
    if (sd_path_get_playlists(playlists_dir, sizeof(playlists_dir)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get playlists directory path");
        return ESP_FAIL;
    }
    
    // Create playlists directory if it doesn't exist
    struct stat st;
    if (stat(playlists_dir, &st) != 0) {
        ESP_LOGI(TAG, "Creating playlists directory: %s", playlists_dir);
        if (mkdir(playlists_dir, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create playlists directory");
            return ESP_FAIL;
        }
    }
    
    // Best-effort: rebuild vault refcount sidecars from cached playlists.
    (void)rebuild_vault_refcounts_from_playlists();

    ESP_LOGI(TAG, "Playlist manager initialized");
    return ESP_OK;
}

void playlist_manager_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing playlist manager");
    
    if (s_current_playlist) {
        playlist_free(s_current_playlist);
        s_current_playlist = NULL;
    }
    s_current_playlist_id = -1;
}

esp_err_t playlist_get(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist)
{
    if (!out_playlist) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if we have this playlist cached
    if (s_current_playlist && s_current_playlist_id == post_id) {
        ESP_LOGD(TAG, "Returning cached playlist %ld", post_id);
        *out_playlist = s_current_playlist;
        return ESP_OK;
    }
    
    // Free old cached playlist
    if (s_current_playlist) {
        ESP_LOGD(TAG, "Releasing old cached playlist %ld", s_current_playlist_id);
        playlist_free(s_current_playlist);
        s_current_playlist = NULL;
    }
    
    // Try to load from disk
    playlist_metadata_t *playlist = NULL;
    esp_err_t err = playlist_load_from_disk(post_id, &playlist);
    
    if (err != ESP_OK || !playlist) {
        ESP_LOGI(TAG, "Playlist %ld not in cache, fetching from server", post_id);
        err = playlist_fetch_from_server(post_id, pe, &playlist);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to fetch playlist %ld from server: %s", 
                     post_id, esp_err_to_name(err));
            return err;
        }
        
        // Save to disk
        playlist_save_to_disk(playlist);
    }
    
    // Cache it
    s_current_playlist = playlist;
    s_current_playlist_id = post_id;
    *out_playlist = playlist;
    
    ESP_LOGI(TAG, "Loaded playlist %ld: %ld total artworks, %ld loaded, %ld available",
             post_id, playlist->total_artworks, playlist->loaded_artworks, 
             playlist->available_artworks);
    
    return ESP_OK;
}

void playlist_release(playlist_metadata_t *playlist)
{
    // For now, we keep the current playlist cached
    // In the future, could implement LRU cache with multiple playlists
    (void)playlist;
}

bool playlist_needs_update(int32_t post_id, time_t server_modified_at)
{
    playlist_metadata_t *playlist = NULL;
    esp_err_t err = playlist_load_from_disk(post_id, &playlist);
    
    if (err != ESP_OK || !playlist) {
        return true;  // Not cached, needs fetch
    }
    
    bool needs_update = difftime(server_modified_at, playlist->metadata_modified_at) > 0;
    
    if (playlist != s_current_playlist) {
        playlist_free(playlist);
    }
    
    return needs_update;
}

esp_err_t playlist_queue_update(int32_t post_id)
{
    // TODO: Implement background update queue
    // For now, just log
    ESP_LOGI(TAG, "Queued update for playlist %ld (not yet implemented)", post_id);
    return ESP_OK;
}

esp_err_t playlist_get_artwork(playlist_metadata_t *playlist, uint32_t index, 
                               artwork_ref_t **out_artwork)
{
    if (!playlist || !out_artwork) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index >= (uint32_t)playlist->loaded_artworks) {
        ESP_LOGE(TAG, "Artwork index %lu out of bounds (loaded: %ld)", 
                 index, playlist->loaded_artworks);
        return ESP_ERR_INVALID_ARG;
    }
    
    *out_artwork = &playlist->artworks[index];
    return ESP_OK;
}

esp_err_t playlist_update_artwork_status(int32_t post_id, int32_t artwork_post_id, bool downloaded)
{
    // Find playlist (check cache first)
    playlist_metadata_t *playlist = NULL;
    bool is_cached = (s_current_playlist && s_current_playlist_id == post_id);
    
    if (is_cached) {
        playlist = s_current_playlist;
    } else {
        esp_err_t err = playlist_load_from_disk(post_id, &playlist);
        if (err != ESP_OK) {
            return err;
        }
    }
    
    // Find artwork and update status
    bool found = false;
    int32_t old_available = playlist->available_artworks;
    
    for (int32_t i = 0; i < playlist->loaded_artworks; i++) {
        if (playlist->artworks[i].post_id == artwork_post_id) {
            playlist->artworks[i].downloaded = downloaded;
            // available_artworks is informational only; keep it as "downloaded count"
            int32_t cnt = 0;
            for (int32_t j = 0; j < playlist->loaded_artworks; j++) {
                if (playlist->artworks[j].downloaded) cnt++;
            }
            playlist->available_artworks = cnt;
            
            found = true;
            break;
        }
    }
    
    if (!found) {
        ESP_LOGW(TAG, "Artwork %ld not found in playlist %ld", artwork_post_id, post_id);
        if (!is_cached && playlist) {
            playlist_free(playlist);
        }
        return ESP_ERR_NOT_FOUND;
    }
    
    // Save if availability changed
    if (old_available != playlist->available_artworks) {
        ESP_LOGI(TAG, "Playlist %ld availability: %ld -> %ld", 
                 post_id, old_available, playlist->available_artworks);
        playlist_save_to_disk(playlist);
    }
    
    if (!is_cached && playlist) {
        playlist_free(playlist);
    }
    
    return ESP_OK;
}

esp_err_t playlist_get_cache_path(int32_t post_id, char *out_path, size_t out_path_len)
{
    if (!out_path || out_path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char playlists_dir[128];
    if (sd_path_get_playlists(playlists_dir, sizeof(playlists_dir)) != ESP_OK) {
        return ESP_FAIL;
    }
    
    int written = snprintf(out_path, out_path_len, "%s/%ld.json", playlists_dir, post_id);
    if (written < 0 || (size_t)written >= out_path_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    return ESP_OK;
}

esp_err_t playlist_load_from_disk(int32_t post_id, playlist_metadata_t **out_playlist)
{
    if (!out_playlist) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char filepath[128];
    esp_err_t err = playlist_get_cache_path(post_id, filepath, sizeof(filepath));
    if (err != ESP_OK) {
        return err;
    }
    
    return load_playlist_json(filepath, out_playlist);
}

esp_err_t playlist_save_to_disk(playlist_metadata_t *playlist)
{
    if (!playlist) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char filepath[128];
    esp_err_t err = playlist_get_cache_path(playlist->post_id, filepath, sizeof(filepath));
    if (err != ESP_OK) {
        return err;
    }
    
    esp_err_t save_err = save_playlist_json(playlist, filepath);
    if (save_err == ESP_OK) {
        // Update refcount sidecars in a single pass (avoids per-playlist incremental drift).
        (void)rebuild_vault_refcounts_from_playlists();
    }
    return save_err;
}

// Continued in next part...

esp_err_t playlist_fetch_from_server(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist)
{
    if (!out_playlist) {
        return ESP_ERR_INVALID_ARG;
    }

    // Fetch post via Makapix API
    makapix_post_t post = {0};
    uint16_t pe16 = (pe > 1023) ? 1023 : (uint16_t)pe;
    esp_err_t err = makapix_api_get_post(post_id, true, pe16, &post);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "makapix_api_get_post failed for %ld: %s", post_id, esp_err_to_name(err));
        return err;
    }

    if (post.kind != MAKAPIX_POST_KIND_PLAYLIST) {
        if (post.artworks) free(post.artworks);
        ESP_LOGE(TAG, "Post %ld is not a playlist", post_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    playlist_metadata_t *playlist = (playlist_metadata_t *)calloc(1, sizeof(playlist_metadata_t));
    if (!playlist) {
        if (post.artworks) free(post.artworks);
        return ESP_ERR_NO_MEM;
    }

    playlist->post_id = post.post_id;
    playlist->total_artworks = post.total_artworks;
    playlist->loaded_artworks = (int32_t)post.artworks_count;
    playlist->available_artworks = 0;
    playlist->dwell_time_ms = post.playlist_dwell_time_ms;
    playlist->metadata_modified_at = parse_iso8601_utc(post.metadata_modified_at);

    if (post.artworks_count > 0 && post.artworks) {
        playlist->artworks = (artwork_ref_t *)calloc(post.artworks_count, sizeof(artwork_ref_t));
        if (!playlist->artworks) {
            free(playlist);
            free(post.artworks);
            return ESP_ERR_NO_MEM;
        }

        for (size_t i = 0; i < post.artworks_count; i++) {
            const makapix_artwork_t *src = &post.artworks[i];
            artwork_ref_t *dst = &playlist->artworks[i];
            memset(dst, 0, sizeof(*dst));

            dst->post_id = src->post_id;
            strncpy(dst->storage_key, src->storage_key, sizeof(dst->storage_key) - 1);
            strlcpy(dst->art_url, src->art_url, sizeof(dst->art_url));
            dst->type = asset_type_from_url(src->art_url);
            dst->dwell_time_ms = src->dwell_time_ms;
            dst->width = (uint16_t)src->width;
            dst->height = (uint16_t)src->height;
            dst->frame_count = (uint16_t)src->frame_count;
            dst->has_transparency = src->has_transparency;
            dst->metadata_modified_at = parse_iso8601_utc(src->metadata_modified_at);
            dst->artwork_modified_at = parse_iso8601_utc(src->artwork_modified_at);

            char vault_path[512];
            build_vault_path_from_storage_key_uuid(src->storage_key, src->art_url, vault_path, sizeof(vault_path));
            struct stat st;
            dst->downloaded = (stat(vault_path, &st) == 0);
            strlcpy(dst->filepath, vault_path, sizeof(dst->filepath));
        }

        // available_artworks is informational only; keep it as "downloaded count"
        int32_t cnt = 0;
        for (int32_t i = 0; i < playlist->loaded_artworks; i++) {
            if (playlist->artworks[i].downloaded) cnt++;
        }
        playlist->available_artworks = cnt;
    }

    if (post.artworks) {
        free(post.artworks);
        post.artworks = NULL;
    }

    *out_playlist = playlist;
    return ESP_OK;
}

void playlist_free(playlist_metadata_t *playlist)
{
    if (!playlist) {
        return;
    }
    
    if (playlist->artworks) {
        free(playlist->artworks);
    }
    
    free(playlist);
}

// ============================================================================
// JSON Serialization/Deserialization
// ============================================================================

static esp_err_t parse_artwork_from_json(cJSON *artwork_json, artwork_ref_t *out_artwork)
{
    if (!artwork_json || !out_artwork) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(out_artwork, 0, sizeof(artwork_ref_t));
    
    // Required fields
    cJSON *post_id = cJSON_GetObjectItem(artwork_json, "post_id");
    cJSON *storage_key = cJSON_GetObjectItem(artwork_json, "storage_key");
    cJSON *art_url = cJSON_GetObjectItem(artwork_json, "art_url");
    
    if (!post_id || !cJSON_IsNumber(post_id)) {
        ESP_LOGE(TAG, "Missing or invalid post_id");
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_key || !cJSON_IsString(storage_key)) {
        ESP_LOGE(TAG, "Missing or invalid storage_key");
        return ESP_ERR_INVALID_ARG;
    }
    if (!art_url || !cJSON_IsString(art_url)) {
        ESP_LOGE(TAG, "Missing or invalid art_url");
        return ESP_ERR_INVALID_ARG;
    }
    
    out_artwork->post_id = cJSON_GetNumberValue(post_id);
    out_artwork->filepath[0] = '\0';
    strncpy(out_artwork->storage_key, cJSON_GetStringValue(storage_key), 
            sizeof(out_artwork->storage_key) - 1);
    strncpy(out_artwork->art_url, cJSON_GetStringValue(art_url), 
            sizeof(out_artwork->art_url) - 1);
    
    // Optional fields
    cJSON *dwell_time = cJSON_GetObjectItem(artwork_json, "dwell_time_ms");
    if (dwell_time && cJSON_IsNumber(dwell_time)) {
        out_artwork->dwell_time_ms = (uint32_t)cJSON_GetNumberValue(dwell_time);
    }
    
    cJSON *width = cJSON_GetObjectItem(artwork_json, "width");
    if (width && cJSON_IsNumber(width)) {
        out_artwork->width = (uint16_t)cJSON_GetNumberValue(width);
    }
    
    cJSON *height = cJSON_GetObjectItem(artwork_json, "height");
    if (height && cJSON_IsNumber(height)) {
        out_artwork->height = (uint16_t)cJSON_GetNumberValue(height);
    }
    
    cJSON *frame_count = cJSON_GetObjectItem(artwork_json, "frame_count");
    if (frame_count && cJSON_IsNumber(frame_count)) {
        out_artwork->frame_count = (uint16_t)cJSON_GetNumberValue(frame_count);
    }
    
    cJSON *has_transparency = cJSON_GetObjectItem(artwork_json, "has_transparency");
    if (has_transparency && cJSON_IsBool(has_transparency)) {
        out_artwork->has_transparency = cJSON_IsTrue(has_transparency);
    }
    
    // Parse timestamps
    cJSON *metadata_modified = cJSON_GetObjectItem(artwork_json, "metadata_modified_at");
    if (metadata_modified && cJSON_IsString(metadata_modified)) {
        // TODO: Parse ISO 8601 timestamp to time_t
        // For now, just set to current time
        out_artwork->metadata_modified_at = time(NULL);
    }
    
    cJSON *artwork_modified = cJSON_GetObjectItem(artwork_json, "artwork_modified_at");
    if (artwork_modified && cJSON_IsString(artwork_modified)) {
        out_artwork->artwork_modified_at = time(NULL);
    }
    
    // Download status (persisted by writer)
    cJSON *downloaded = cJSON_GetObjectItem(artwork_json, "downloaded");
    out_artwork->downloaded = cJSON_IsBool(downloaded) ? cJSON_IsTrue(downloaded) : false;

    // Optional local filepath (vault or SD)
    cJSON *filepath = cJSON_GetObjectItem(artwork_json, "filepath");
    if (filepath && cJSON_IsString(filepath)) {
        strncpy(out_artwork->filepath, cJSON_GetStringValue(filepath), sizeof(out_artwork->filepath) - 1);
    }
    
    // Determine file type from art_url extension
    out_artwork->type = asset_type_from_url(out_artwork->art_url);
    
    return ESP_OK;
}

static esp_err_t parse_playlist_from_json(cJSON *json, playlist_metadata_t **out_playlist)
{
    if (!json || !out_playlist) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Required fields
    cJSON *post_id = cJSON_GetObjectItem(json, "post_id");
    cJSON *total_artworks = cJSON_GetObjectItem(json, "total_artworks");
    
    if (!post_id || !cJSON_IsNumber(post_id)) {
        ESP_LOGE(TAG, "Missing or invalid post_id");
        return ESP_ERR_INVALID_ARG;
    }
    if (!total_artworks || !cJSON_IsNumber(total_artworks)) {
        ESP_LOGE(TAG, "Missing or invalid total_artworks");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Allocate playlist
    playlist_metadata_t *playlist = (playlist_metadata_t *)calloc(1, sizeof(playlist_metadata_t));
    if (!playlist) {
        return ESP_ERR_NO_MEM;
    }
    
    playlist->post_id = (int32_t)cJSON_GetNumberValue(post_id);
    playlist->total_artworks = (int32_t)cJSON_GetNumberValue(total_artworks);
    
    // Optional fields
    cJSON *dwell_time = cJSON_GetObjectItem(json, "dwell_time_ms");
    if (dwell_time && cJSON_IsNumber(dwell_time)) {
        playlist->dwell_time_ms = (uint32_t)cJSON_GetNumberValue(dwell_time);
    }
    
    cJSON *metadata_modified = cJSON_GetObjectItem(json, "metadata_modified_at");
    if (metadata_modified && cJSON_IsString(metadata_modified)) {
        playlist->metadata_modified_at = time(NULL);  // TODO: Parse ISO 8601
    }
    
    // Parse artworks array
    cJSON *artworks = cJSON_GetObjectItem(json, "artworks");
    if (artworks && cJSON_IsArray(artworks)) {
        int artwork_count = cJSON_GetArraySize(artworks);
        if (artwork_count > 0) {
            playlist->artworks = (artwork_ref_t *)calloc(artwork_count, sizeof(artwork_ref_t));
            if (!playlist->artworks) {
                free(playlist);
                return ESP_ERR_NO_MEM;
            }
            
            int loaded = 0;
            
            for (int i = 0; i < artwork_count; i++) {
                cJSON *artwork_json = cJSON_GetArrayItem(artworks, i);
                if (artwork_json) {
                    esp_err_t err = parse_artwork_from_json(artwork_json, &playlist->artworks[loaded]);
                    if (err == ESP_OK) {
                        loaded++;
                    } else {
                        ESP_LOGW(TAG, "Failed to parse artwork at index %d", i);
                    }
                }
            }
            
            playlist->loaded_artworks = loaded;
            // available_artworks is informational only; keep it as "downloaded count"
            int cnt = 0;
            for (int i = 0; i < loaded; i++) {
                if (playlist->artworks[i].downloaded) cnt++;
            }
            playlist->available_artworks = cnt;
        }
    }
    
    *out_playlist = playlist;
    return ESP_OK;
}

static esp_err_t save_playlist_json(playlist_metadata_t *playlist, const char *filepath)
{
    if (!playlist || !filepath) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(root, "post_id", playlist->post_id);
    cJSON_AddNumberToObject(root, "total_artworks", playlist->total_artworks);
    cJSON_AddNumberToObject(root, "loaded_artworks", playlist->loaded_artworks);
    cJSON_AddNumberToObject(root, "available_artworks", playlist->available_artworks);
    cJSON_AddNumberToObject(root, "dwell_time_ms", playlist->dwell_time_ms);
    
    // Add artworks array
    cJSON *artworks = cJSON_AddArrayToObject(root, "artworks");
    if (artworks) {
        for (int32_t i = 0; i < playlist->loaded_artworks; i++) {
            artwork_ref_t *artwork = &playlist->artworks[i];
            cJSON *artwork_json = cJSON_CreateObject();
            
            cJSON_AddNumberToObject(artwork_json, "post_id", artwork->post_id);
            if (artwork->filepath[0] != '\0') {
                cJSON_AddStringToObject(artwork_json, "filepath", artwork->filepath);
            }
            cJSON_AddStringToObject(artwork_json, "storage_key", artwork->storage_key);
            cJSON_AddStringToObject(artwork_json, "art_url", artwork->art_url);
            cJSON_AddNumberToObject(artwork_json, "dwell_time_ms", artwork->dwell_time_ms);
            cJSON_AddNumberToObject(artwork_json, "width", artwork->width);
            cJSON_AddNumberToObject(artwork_json, "height", artwork->height);
            cJSON_AddNumberToObject(artwork_json, "frame_count", artwork->frame_count);
            cJSON_AddBoolToObject(artwork_json, "has_transparency", artwork->has_transparency);
            cJSON_AddBoolToObject(artwork_json, "downloaded", artwork->downloaded);
            
            cJSON_AddItemToArray(artworks, artwork_json);
        }
    }
    
    // Write to file
    char *json_string = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (!json_string) {
        return ESP_ERR_NO_MEM;
    }
    
    FILE *f = fopen(filepath, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        free(json_string);
        return ESP_FAIL;
    }
    
    size_t written = fwrite(json_string, 1, strlen(json_string), f);
    fclose(f);
    free(json_string);
    
    if (written == 0) {
        ESP_LOGE(TAG, "Failed to write playlist JSON");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Saved playlist %ld to %s", playlist->post_id, filepath);
    return ESP_OK;
}

static esp_err_t load_playlist_json(const char *filepath, playlist_metadata_t **out_playlist)
{
    if (!filepath || !out_playlist) {
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGD(TAG, "Playlist cache file not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize <= 0 || fsize > 1024 * 1024) {  // Max 1 MB
        ESP_LOGE(TAG, "Invalid file size: %ld", fsize);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Read file
    char *json_string = (char *)malloc(fsize + 1);
    if (!json_string) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_size = fread(json_string, 1, fsize, f);
    fclose(f);
    json_string[read_size] = '\0';
    
    // Parse JSON
    cJSON *root = cJSON_Parse(json_string);
    free(json_string);
    
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse playlist JSON from %s", filepath);
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t err = parse_playlist_from_json(root, out_playlist);
    cJSON_Delete(root);
    
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Loaded playlist from %s", filepath);
    }
    
    return err;
}

