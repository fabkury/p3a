#include "playlist_manager.h"
#include "vault_storage.h"
#include "makapix_api.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "playlist_mgr";

#define PLAYLISTS_DIR "/sdcard/playlists"

// Current cached playlist
static playlist_metadata_t *s_current_playlist = NULL;
static int32_t s_current_playlist_id = -1;

// Forward declarations
static esp_err_t parse_artwork_from_json(cJSON *artwork_json, artwork_ref_t *out_artwork);
static esp_err_t parse_playlist_from_json(cJSON *json, playlist_metadata_t **out_playlist);
static esp_err_t save_playlist_json(playlist_metadata_t *playlist, const char *filepath);
static esp_err_t load_playlist_json(const char *filepath, playlist_metadata_t **out_playlist);

esp_err_t playlist_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing playlist manager");
    
    // Create playlists directory if it doesn't exist
    struct stat st;
    if (stat(PLAYLISTS_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating playlists directory: %s", PLAYLISTS_DIR);
        if (mkdir(PLAYLISTS_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create playlists directory");
            return ESP_FAIL;
        }
    }
    
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
            bool was_downloaded = playlist->artworks[i].downloaded;
            playlist->artworks[i].downloaded = downloaded;
            
            // Update available count
            if (downloaded && !was_downloaded) {
                playlist->available_artworks++;
            } else if (!downloaded && was_downloaded) {
                playlist->available_artworks--;
            }
            
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
    
    int written = snprintf(out_path, out_path_len, "%s/%ld.json", PLAYLISTS_DIR, post_id);
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
    
    return save_playlist_json(playlist, filepath);
}

// Continued in next part...

esp_err_t playlist_fetch_from_server(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist)
{
    // TODO: Implement actual server fetch via makapix_api
    // This is a placeholder that will be implemented when makapix_api.c is updated
    
    ESP_LOGW(TAG, "playlist_fetch_from_server not yet implemented for post_id %ld", post_id);
    return ESP_ERR_NOT_SUPPORTED;
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
    
    // Check if artwork is downloaded (in vault)
    // TODO: Check vault for storage_key
    out_artwork->downloaded = false;
    
    // Determine file type from storage_key extension
    const char *ext = strrchr(out_artwork->storage_key, '.');
    if (ext) {
        if (strcasecmp(ext, ".webp") == 0) {
            out_artwork->type = ASSET_TYPE_WEBP;
        } else if (strcasecmp(ext, ".gif") == 0) {
            out_artwork->type = ASSET_TYPE_GIF;
        } else if (strcasecmp(ext, ".png") == 0) {
            out_artwork->type = ASSET_TYPE_PNG;
        } else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
            out_artwork->type = ASSET_TYPE_JPEG;
        }
    }
    
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
            int available = 0;
            
            for (int i = 0; i < artwork_count; i++) {
                cJSON *artwork_json = cJSON_GetArrayItem(artworks, i);
                if (artwork_json) {
                    esp_err_t err = parse_artwork_from_json(artwork_json, &playlist->artworks[loaded]);
                    if (err == ESP_OK) {
                        if (playlist->artworks[loaded].downloaded) {
                            available++;
                        }
                        loaded++;
                    } else {
                        ESP_LOGW(TAG, "Failed to parse artwork at index %d", i);
                    }
                }
            }
            
            playlist->loaded_artworks = loaded;
            playlist->available_artworks = available;
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

