#ifndef PLAYLIST_MANAGER_H
#define PLAYLIST_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "sdcard_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum artworks per playlist
 */
#define PLAYLIST_MAX_ARTWORKS 1024

/**
 * @brief Artwork reference within a playlist
 * 
 * Contains metadata and download status for a single artwork in a playlist.
 */
typedef struct {
    int32_t post_id;
    char filepath[256];         // Local path to artwork file (vault or SD). May be empty if unknown.
    char storage_key[96];      // SHA256-based key in vault
    char art_url[256];         // Download URL
    asset_type_t type;         // File type (WebP, GIF, etc)
    uint32_t dwell_time_ms;    // 0 = use playlist default
    time_t metadata_modified_at;
    time_t artwork_modified_at;
    bool downloaded;           // Is artwork file in vault?
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    bool has_transparency;
} artwork_ref_t;

/**
 * @brief Playlist metadata
 * 
 * Represents a playlist post with its collection of artworks.
 * Cached in memory for current playlist, stored on disk for others.
 */
typedef struct {
    int32_t post_id;
    int32_t total_artworks;      // Server's total count
    int32_t loaded_artworks;     // How many we have metadata for
    int32_t available_artworks;  // How many are fully downloaded
    uint32_t dwell_time_ms;      // Default for artworks in this playlist
    time_t metadata_modified_at;
    artwork_ref_t *artworks;     // Array of artwork references (dynamically allocated)
} playlist_metadata_t;

/**
 * @brief Initialize the playlist manager
 * 
 * Creates /sdcard/playlists/ directory if needed.
 * 
 * @return ESP_OK on success
 */
esp_err_t playlist_manager_init(void);

/**
 * @brief Deinitialize and free resources
 */
void playlist_manager_deinit(void);

/**
 * @brief Get playlist metadata
 * 
 * Returns cached playlist if post_id matches current, otherwise loads from disk.
 * If not on disk or stale, fetches from server.
 * 
 * @param post_id Playlist post ID
 * @param pe Playlist expansion (0 = all, 1-1023 = first N artworks)
 * @param out_playlist Pointer to receive playlist metadata (do NOT free, owned by manager)
 * @return ESP_OK on success
 */
esp_err_t playlist_get(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist);

/**
 * @brief Release reference to cached playlist
 * 
 * Call when done using a playlist returned by playlist_get().
 * Manager may choose to free it if not currently active.
 * 
 * @param playlist Playlist to release
 */
void playlist_release(playlist_metadata_t *playlist);

/**
 * @brief Check if playlist needs update from server
 * 
 * @param post_id Playlist post ID
 * @param server_modified_at Server's metadata_modified_at timestamp
 * @return true if local cache is stale
 */
bool playlist_needs_update(int32_t post_id, time_t server_modified_at);

/**
 * @brief Queue background update for a playlist
 * 
 * Non-blocking. Update happens asynchronously.
 * 
 * @param post_id Playlist post ID
 * @return ESP_OK on success (update queued)
 */
esp_err_t playlist_queue_update(int32_t post_id);

/**
 * @brief Get specific artwork from playlist
 * 
 * @param playlist Playlist metadata
 * @param index Artwork index (0-based)
 * @param out_artwork Pointer to receive artwork reference (do NOT free)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of bounds
 */
esp_err_t playlist_get_artwork(playlist_metadata_t *playlist, uint32_t index, 
                               artwork_ref_t **out_artwork);

/**
 * @brief Update artwork download status
 * 
 * Call when an artwork finishes downloading to update availability.
 * 
 * @param post_id Playlist post ID
 * @param artwork_post_id Artwork's post ID
 * @param downloaded New download status
 * @return ESP_OK on success
 */
esp_err_t playlist_update_artwork_status(int32_t post_id, int32_t artwork_post_id, bool downloaded);

/**
 * @brief Load playlist from disk cache
 * 
 * @param post_id Playlist post ID
 * @param out_playlist Pointer to receive allocated playlist (caller must free with playlist_free)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not cached
 */
esp_err_t playlist_load_from_disk(int32_t post_id, playlist_metadata_t **out_playlist);

/**
 * @brief Save playlist to disk cache
 * 
 * @param playlist Playlist to save
 * @return ESP_OK on success
 */
esp_err_t playlist_save_to_disk(playlist_metadata_t *playlist);

/**
 * @brief Fetch playlist from server via Makapix API
 * 
 * @param post_id Playlist post ID
 * @param pe Playlist expansion
 * @param out_playlist Pointer to receive allocated playlist (caller must free with playlist_free)
 * @return ESP_OK on success
 */
esp_err_t playlist_fetch_from_server(int32_t post_id, uint32_t pe, playlist_metadata_t **out_playlist);

/**
 * @brief Free playlist metadata
 * 
 * Frees artworks array and playlist structure.
 * 
 * @param playlist Playlist to free
 */
void playlist_free(playlist_metadata_t *playlist);

/**
 * @brief Get path to playlist cache file
 * 
 * @param post_id Playlist post ID
 * @param out_path Buffer to receive path
 * @param out_path_len Buffer length
 * @return ESP_OK on success
 */
esp_err_t playlist_get_cache_path(int32_t post_id, char *out_path, size_t out_path_len);

#ifdef __cplusplus
}
#endif

#endif // PLAYLIST_MANAGER_H
