#include "sdcard_channel.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "sdcard_channel";

// Internal channel state
static struct {
    sdcard_post_t *posts;
    size_t count;
    size_t capacity;
    char *animations_dir;
    sdcard_sort_order_t current_sort_order;
    bool initialized;
} s_channel = {0};

static asset_type_t get_asset_type(const char *filename)
{
    size_t len = strlen(filename);
    // Check longer extensions first (e.g., .jpeg before .jpg), all comparisons are case-insensitive
    if (len >= 5 && strcasecmp(filename + len - 5, ".webp") == 0) {
        return ASSET_TYPE_WEBP;
    }
    if (len >= 5 && strcasecmp(filename + len - 5, ".jpeg") == 0) {
        return ASSET_TYPE_JPEG; // JPEG (prefer .jpg but accept .jpeg)
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".gif") == 0) {
        return ASSET_TYPE_GIF;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".png") == 0) {
        return ASSET_TYPE_PNG;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".jpg") == 0) {
        return ASSET_TYPE_JPEG; // JPEG (canonical extension)
    }
    return ASSET_TYPE_WEBP;
}

static bool is_animation_file(const char *filename)
{
    size_t len = strlen(filename);
    return (len >= 5 && strcasecmp(filename + len - 5, ".webp") == 0) ||
           (len >= 4 && strcasecmp(filename + len - 4, ".gif") == 0) ||
           (len >= 4 && strcasecmp(filename + len - 4, ".png") == 0) ||
           (len >= 4 && strcasecmp(filename + len - 4, ".jpg") == 0) ||
           (len >= 5 && strcasecmp(filename + len - 5, ".jpeg") == 0);
}

static void free_post(sdcard_post_t *post)
{
    if (!post) return;
    free(post->name);
    free(post->filepath);
    memset(post, 0, sizeof(sdcard_post_t));
}

static void free_all_posts(void)
{
    if (s_channel.posts) {
        for (size_t i = 0; i < s_channel.count; i++) {
            free_post(&s_channel.posts[i]);
        }
        free(s_channel.posts);
        s_channel.posts = NULL;
    }
    s_channel.count = 0;
    s_channel.capacity = 0;
}

static int compare_posts_by_name(const void *a, const void *b)
{
    const sdcard_post_t *pa = (const sdcard_post_t *)a;
    const sdcard_post_t *pb = (const sdcard_post_t *)b;
    return strcasecmp(pa->name, pb->name);
}

static int compare_posts_by_date(const void *a, const void *b)
{
    const sdcard_post_t *pa = (const sdcard_post_t *)a;
    const sdcard_post_t *pb = (const sdcard_post_t *)b;
    // Newest first (descending order)
    if (pa->created_at > pb->created_at) return -1;
    if (pa->created_at < pb->created_at) return 1;
    return 0;
}

static void sort_posts(sdcard_sort_order_t order)
{
    if (s_channel.count == 0) {
        s_channel.current_sort_order = order;
        return;
    }

    if (s_channel.current_sort_order == order) {
        // Already sorted in this order
        return;
    }

    if (order == SDCARD_SORT_BY_NAME) {
        qsort(s_channel.posts, s_channel.count, sizeof(sdcard_post_t), compare_posts_by_name);
    } else if (order == SDCARD_SORT_BY_DATE) {
        qsort(s_channel.posts, s_channel.count, sizeof(sdcard_post_t), compare_posts_by_date);
    }

    s_channel.current_sort_order = order;
    ESP_LOGI(TAG, "Sorted %zu posts by %s", s_channel.count,
             order == SDCARD_SORT_BY_NAME ? "name" : "date");
}

esp_err_t sdcard_channel_init(void)
{
    if (s_channel.initialized) {
        ESP_LOGW(TAG, "Channel already initialized");
        return ESP_OK;
    }

    memset(&s_channel, 0, sizeof(s_channel));
    s_channel.current_sort_order = SDCARD_SORT_BY_NAME; // Default unset state
    s_channel.initialized = true;

    ESP_LOGI(TAG, "SD card channel initialized");
    return ESP_OK;
}

void sdcard_channel_deinit(void)
{
    if (!s_channel.initialized) {
        return;
    }

    free_all_posts();
    free(s_channel.animations_dir);
    s_channel.animations_dir = NULL;
    s_channel.initialized = false;

    ESP_LOGI(TAG, "SD card channel deinitialized");
}

esp_err_t sdcard_channel_refresh(const char *animations_dir)
{
    if (!s_channel.initialized) {
        ESP_LOGE(TAG, "Channel not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const char *dir_path = animations_dir ? animations_dir : ANIMATIONS_DEFAULT_DIR;
    ESP_LOGI(TAG, "Refreshing channel from directory: %s", dir_path);

    // Free existing posts
    free_all_posts();

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", dir_path, errno);
        return ESP_FAIL;
    }

    // First pass: count animation files (up to MAX_POSTS)
    struct dirent *entry;
    size_t file_count = 0;
    while ((entry = readdir(dir)) != NULL && file_count < SDCARD_CHANNEL_MAX_POSTS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (is_animation_file(entry->d_name)) {
            file_count++;
        }
    }

    if (file_count == 0) {
        ESP_LOGW(TAG, "No animation files found in %s", dir_path);
        closedir(dir);
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate posts array
    s_channel.capacity = file_count;
    s_channel.posts = (sdcard_post_t *)calloc(file_count, sizeof(sdcard_post_t));
    if (!s_channel.posts) {
        ESP_LOGE(TAG, "Failed to allocate posts array");
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    // Second pass: collect file metadata
    rewinddir(dir);
    size_t idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < SDCARD_CHANNEL_MAX_POSTS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (!is_animation_file(entry->d_name)) {
            continue;
        }

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        sdcard_post_t *post = &s_channel.posts[idx];

        // Allocate and copy name
        size_t name_len = strlen(entry->d_name);
        post->name = (char *)malloc(name_len + 1);
        if (!post->name) {
            ESP_LOGE(TAG, "Failed to allocate post name");
            // Clean up what we've allocated so far
            for (size_t i = 0; i < idx; i++) {
                free_post(&s_channel.posts[i]);
            }
            free(s_channel.posts);
            s_channel.posts = NULL;
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        strcpy(post->name, entry->d_name);

        // Allocate and copy filepath
        size_t path_len = strlen(full_path);
        post->filepath = (char *)malloc(path_len + 1);
        if (!post->filepath) {
            ESP_LOGE(TAG, "Failed to allocate post filepath");
            free(post->name);
            for (size_t i = 0; i < idx; i++) {
                free_post(&s_channel.posts[i]);
            }
            free(s_channel.posts);
            s_channel.posts = NULL;
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        strcpy(post->filepath, full_path);

        // Set metadata
        post->created_at = st.st_mtime; // Use modification time as creation date
        post->type = get_asset_type(entry->d_name);
        post->healthy = true;

        idx++;
    }
    closedir(dir);

    s_channel.count = idx;

    // Update animations directory
    free(s_channel.animations_dir);
    size_t dir_len = strlen(dir_path);
    s_channel.animations_dir = (char *)malloc(dir_len + 1);
    if (!s_channel.animations_dir) {
        ESP_LOGE(TAG, "Failed to allocate directory path");
        free_all_posts();
        return ESP_ERR_NO_MEM;
    }
    strcpy(s_channel.animations_dir, dir_path);

    // Reset sort order (will be set on first query)
    s_channel.current_sort_order = SDCARD_SORT_BY_NAME; // Mark as unset

    ESP_LOGI(TAG, "Refreshed channel: %zu posts from %s", s_channel.count, dir_path);
    return ESP_OK;
}

esp_err_t sdcard_channel_query(const sdcard_query_t *query, sdcard_query_result_t *result)
{
    if (!s_channel.initialized) {
        ESP_LOGE(TAG, "Channel not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!query || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate query
    if (query->count > SDCARD_CHANNEL_PAGE_SIZE) {
        ESP_LOGW(TAG, "Query count %zu exceeds max page size %d, clamping", 
                 query->count, SDCARD_CHANNEL_PAGE_SIZE);
    }
    size_t requested_count = query->count > SDCARD_CHANNEL_PAGE_SIZE ? 
                             SDCARD_CHANNEL_PAGE_SIZE : query->count;

    // Sort if needed
    sort_posts(query->sort_order);

    // Calculate actual return count
    size_t available = s_channel.count;
    if (query->offset >= available) {
        result->posts = NULL;
        result->count = 0;
        result->total = s_channel.count;
        return ESP_OK;
    }

    size_t remaining = available - query->offset;
    size_t return_count = remaining < requested_count ? remaining : requested_count;

    // Allocate result array
    result->posts = (sdcard_post_t *)malloc(return_count * sizeof(sdcard_post_t));
    if (!result->posts) {
        ESP_LOGE(TAG, "Failed to allocate query result array");
        return ESP_ERR_NO_MEM;
    }

    // Copy posts (shallow copy - caller must not free individual post fields)
    for (size_t i = 0; i < return_count; i++) {
        result->posts[i] = s_channel.posts[query->offset + i];
    }

    result->count = return_count;
    result->total = s_channel.count;

    return ESP_OK;
}

size_t sdcard_channel_get_total_count(void)
{
    return s_channel.initialized ? s_channel.count : 0;
}

esp_err_t sdcard_channel_get_latest_post_date(time_t *out_date)
{
    if (!s_channel.initialized || !out_date) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_channel.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // Ensure sorted by date to find latest
    sort_posts(SDCARD_SORT_BY_DATE);
    
    // First post is newest (descending order)
    *out_date = s_channel.posts[0].created_at;
    return ESP_OK;
}

void sdcard_channel_mark_unhealthy(size_t post_index)
{
    if (!s_channel.initialized || post_index >= s_channel.count) {
        return;
    }

    s_channel.posts[post_index].healthy = false;
    ESP_LOGD(TAG, "Marked post %zu (%s) as unhealthy", post_index, 
             s_channel.posts[post_index].name);
}

esp_err_t sdcard_channel_get_post(size_t post_index, const sdcard_post_t **out_post)
{
    if (!s_channel.initialized || !out_post) {
        return ESP_ERR_INVALID_ARG;
    }

    if (post_index >= s_channel.count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_post = &s_channel.posts[post_index];
    return ESP_OK;
}

