#include "makapix_artwork.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

static const char *TAG = "makapix_artwork";
static const char *VAULT_BASE = "/sdcard/vault";

/**
 * @brief Simple hash function for storage_key to derive folder structure
 */
static uint32_t hash_string(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

/**
 * @brief Ensure vault directory structure exists
 */
static esp_err_t ensure_vault_dirs(const char *dir1, const char *dir2)
{
    char path[256];
    
    // Create first level directory
    snprintf(path, sizeof(path), "%s/%s", VAULT_BASE, dir1);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create directory %s", path);
            return ESP_FAIL;
        }
    }

    // Create second level directory
    snprintf(path, sizeof(path), "%s/%s/%s", VAULT_BASE, dir1, dir2);
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create directory %s", path);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

typedef struct {
    FILE *fp;
    makapix_download_progress_cb progress_cb;
    void *progress_ctx;
    size_t content_length;
    size_t bytes_read;
    bool has_length;
    int last_percent;
} http_event_ctx_t;

/**
 * @brief HTTP event handler for download with optional progress
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_event_ctx_t *ctx = (http_event_ctx_t *)evt->user_data;
    if (!ctx) return ESP_FAIL;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;

    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;

    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            ctx->content_length = strtoul(evt->header_value, NULL, 10);
            ctx->has_length = (ctx->content_length > 0);
        }
        break;

    case HTTP_EVENT_ON_DATA:
        // Write data regardless of chunked or non-chunked transfer
        if (ctx->fp && evt->data_len > 0) {
            size_t written = fwrite(evt->data, 1, evt->data_len, ctx->fp);
            if (written != (size_t)evt->data_len) {
                ESP_LOGE(TAG, "Failed to write all data to file");
                return ESP_FAIL;
            }
            ctx->bytes_read += evt->data_len;
            if (ctx->progress_cb) {
                if (ctx->has_length && ctx->content_length > 0) {
                    int percent = (int)((ctx->bytes_read * 100) / ctx->content_length);
                    if (percent != ctx->last_percent) {
                        ctx->last_percent = percent;
                        ctx->progress_cb(ctx->bytes_read, ctx->content_length, ctx->progress_ctx);
                    }
                } else {
                    ctx->progress_cb(ctx->bytes_read, 0, ctx->progress_ctx);
                }
            }
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (ctx->fp) {
            fflush(ctx->fp);
        }
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;

    default:
        break;
    }

    return ESP_OK;
}

esp_err_t makapix_artwork_download_with_progress(const char *art_url, const char *storage_key,
                                                 char *out_path, size_t path_len,
                                                 makapix_download_progress_cb cb, void *user_ctx)
{
    if (!art_url || !storage_key || !out_path || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Ensure vault base directory exists
    struct stat st;
    if (stat(VAULT_BASE, &st) != 0) {
        if (mkdir(VAULT_BASE, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create vault directory");
            return ESP_FAIL;
        }
    }

    // Derive folder structure from hash
    uint32_t hash = hash_string(storage_key);
    char dir1[3], dir2[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)((hash >> 24) & 0xFF));
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)((hash >> 16) & 0xFF));

    // Ensure directories exist
    esp_err_t err = ensure_vault_dirs(dir1, dir2);
    if (err != ESP_OK) {
        return err;
    }

    // Build file path
    snprintf(out_path, path_len, "%s/%s/%s/%s", VAULT_BASE, dir1, dir2, storage_key);

    ESP_LOGI(TAG, "Downloading artwork from %s to %s", art_url, out_path);

    // Open file for writing
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", out_path);
        return ESP_FAIL;
    }

    // Configure HTTP client
    http_event_ctx_t evt_ctx = {
        .fp = fp,
        .progress_cb = cb,
        .progress_ctx = user_ctx,
        .content_length = 0,
        .bytes_read = 0,
        .has_length = false,
        .last_percent = -1,
    };

    esp_http_client_config_t config = {
        .url = art_url,
        .event_handler = http_event_handler,
        .user_data = &evt_ctx,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        fclose(fp);
        unlink(out_path);
        return ESP_ERR_NO_MEM;
    }

    // Perform download
    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(TAG, "Artwork downloaded successfully");
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
            err = ESP_ERR_INVALID_RESPONSE;
            fclose(fp);
            unlink(out_path);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        fclose(fp);
        unlink(out_path);
    }

    if (err == ESP_OK) {
        fclose(fp);
    }

    esp_http_client_cleanup(client);

    return err;
}

esp_err_t makapix_artwork_download(const char *art_url, const char *storage_key, char *out_path, size_t path_len)
{
    return makapix_artwork_download_with_progress(art_url, storage_key, out_path, path_len, NULL, NULL);
}

/**
 * @brief File info structure for cache eviction
 */
typedef struct {
    char path[512];  // Increased buffer size
    time_t mtime;
} file_info_t;

/**
 * @brief Compare function for qsort (oldest first)
 */
static int compare_file_info(const void *a, const void *b)
{
    const file_info_t *fa = (const file_info_t *)a;
    const file_info_t *fb = (const file_info_t *)b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return 0;
}

/**
 * @brief Recursively collect files from vault directory
 */
static size_t collect_files(const char *dir, file_info_t *files, size_t max_files, size_t *count)
{
    DIR *d = opendir(dir);
    if (!d) {
        return *count;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *count < max_files) {
        if (entry->d_name[0] == '.') {
            continue; // Skip . and ..
        }

        char full_path[512];  // Increased buffer size
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(full_path)) {
            continue; // Path too long, skip
        }

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Recursively process subdirectory
                collect_files(full_path, files, max_files, count);
            } else if (S_ISREG(st.st_mode)) {
                // Add file to list
                if (*count < max_files) {
                    snprintf(files[*count].path, sizeof(files[*count].path), "%s", full_path);
                    files[*count].mtime = st.st_mtime;
                    (*count)++;
                }
            }
        }
    }

    closedir(d);
    return *count;
}

esp_err_t makapix_artwork_ensure_cache_limit(size_t max_items)
{
    // Check if vault directory exists
    struct stat st;
    if (stat(VAULT_BASE, &st) != 0) {
        // Vault doesn't exist yet, nothing to evict
        return ESP_OK;
    }

    // Allocate buffer for file list (estimate max needed)
    const size_t max_files = max_items * 2; // Allocate extra space
    file_info_t *files = malloc(sizeof(file_info_t) * max_files);
    if (!files) {
        ESP_LOGE(TAG, "Failed to allocate memory for file list");
        return ESP_ERR_NO_MEM;
    }

    // Collect all files
    size_t file_count = 0;
    collect_files(VAULT_BASE, files, max_files, &file_count);

    ESP_LOGI(TAG, "Found %zu files in vault", file_count);

    if (file_count <= max_items) {
        free(files);
        return ESP_OK; // No eviction needed
    }

    // Sort by modification time (oldest first)
    qsort(files, file_count, sizeof(file_info_t), compare_file_info);

    // Delete oldest files
    size_t to_delete = file_count - max_items;
    size_t deleted = 0;
    for (size_t i = 0; i < to_delete && i < file_count; i++) {
        if (unlink(files[i].path) == 0) {
            deleted++;
            ESP_LOGD(TAG, "Deleted old file: %s", files[i].path);
        } else {
            ESP_LOGW(TAG, "Failed to delete file: %s", files[i].path);
        }
    }

    ESP_LOGI(TAG, "Evicted %zu files from cache", deleted);

    free(files);
    return ESP_OK;
}

