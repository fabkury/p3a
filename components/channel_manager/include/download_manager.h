#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "playlist_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOWNLOAD_PRIORITY_HIGH = 0,
    DOWNLOAD_PRIORITY_MEDIUM = 1,
    DOWNLOAD_PRIORITY_LOW = 2,
} download_priority_t;

typedef struct {
    int32_t playlist_post_id;     // 0 if not from a playlist
    int32_t artwork_post_id;      // Artwork post_id (if known)
    char storage_key[96];
    char art_url[256];
    char filepath[256];           // Expected local path (vault). May be empty.
    download_priority_t priority;
} download_request_t;

esp_err_t download_manager_init(void);
void download_manager_deinit(void);

esp_err_t download_queue(const download_request_t *req);

/**
 * @brief Convenience: enqueue a playlist artwork for download
 */
esp_err_t download_queue_artwork(int32_t playlist_post_id, const artwork_ref_t *artwork, download_priority_t priority);

#ifdef __cplusplus
}
#endif

#endif // DOWNLOAD_MANAGER_H

