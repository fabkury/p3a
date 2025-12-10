#include "sdcard_channel_impl.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "sdcard_channel_impl";

/**
 * @brief Internal SD card channel state
 */
typedef struct {
    struct channel_s base;      // Base channel (must be first)
    
    // Configuration
    char *animations_dir;       // Directory to scan
    
    // Loaded items
    sdcard_post_t *posts;       // Array of posts
    size_t post_count;          // Number of posts
    
    // Playback order
    uint32_t *playback_order;   // Indices into posts array
    size_t order_count;         // Number of items in playback order (after filtering)
    size_t current_pos;         // Current position in playback order
    
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
};

// Helper: get filter flags from asset type
static channel_filter_flags_t get_filter_flags(asset_type_t type)
{
    switch (type) {
        case ASSET_TYPE_GIF:  return CHANNEL_FILTER_FLAG_GIF;
        case ASSET_TYPE_WEBP: return CHANNEL_FILTER_FLAG_WEBP;
        case ASSET_TYPE_PNG:  return CHANNEL_FILTER_FLAG_PNG;
        case ASSET_TYPE_JPEG: return CHANNEL_FILTER_FLAG_JPEG;
        default:              return CHANNEL_FILTER_FLAG_NONE;
    }
}

// Helper: check if a post passes the filter
static bool post_passes_filter(const sdcard_post_t *post, const channel_filter_config_t *filter)
{
    if (!filter) return true;
    
    channel_filter_flags_t flags = get_filter_flags(post->type);
    
    // Check required flags
    if ((flags & filter->required_flags) != filter->required_flags) {
        return false;
    }
    
    // Check excluded flags
    if ((flags & filter->excluded_flags) != 0) {
        return false;
    }
    
    return post->healthy;  // Also filter out unhealthy posts
}

// Helper: fill item ref from post
static void fill_item_ref(const sdcard_post_t *post, uint32_t index, channel_item_ref_t *out)
{
    if (!post || !out) return;
    
    memset(out, 0, sizeof(*out));
    
    if (post->filepath) {
        strncpy(out->filepath, post->filepath, sizeof(out->filepath) - 1);
    }
    
    if (post->name) {
        strncpy(out->storage_key, post->name, sizeof(out->storage_key) - 1);
    }
    
    out->item_index = index;
    out->flags = get_filter_flags(post->type);
}

// Helper: compare by date (newest first)
static int compare_by_date(const void *a, const void *b, void *ctx)
{
    const uint32_t *ia = (const uint32_t *)a;
    const uint32_t *ib = (const uint32_t *)b;
    sdcard_channel_t *ch = (sdcard_channel_t *)ctx;
    
    time_t ta = ch->posts[*ia].created_at;
    time_t tb = ch->posts[*ib].created_at;
    
    if (ta > tb) return -1;
    if (ta < tb) return 1;
    return 0;
}

// Helper: Fisher-Yates shuffle
static void shuffle_order(uint32_t *order, size_t count)
{
    if (count <= 1) return;
    
    for (size_t i = count - 1; i > 0; i--) {
        uint32_t j = esp_random() % (i + 1);
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

// Interface implementation

static esp_err_t sdcard_impl_load(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    if (ch->base.loaded) {
        ESP_LOGW(TAG, "Channel already loaded, refreshing...");
        sdcard_impl_unload(channel);
    }
    
    const char *dir_path = ch->animations_dir ? ch->animations_dir : ANIMATIONS_DEFAULT_DIR;
    ESP_LOGI(TAG, "Loading channel from: %s", dir_path);
    
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", dir_path, errno);
        return ESP_FAIL;
    }
    
    // Count files first
    struct dirent *entry;
    size_t file_count = 0;
    while ((entry = readdir(dir)) != NULL && file_count < SDCARD_CHANNEL_MAX_POSTS) {
        if (entry->d_name[0] == '.') continue;
        
        size_t len = strlen(entry->d_name);
        if ((len >= 5 && strcasecmp(entry->d_name + len - 5, ".webp") == 0) ||
            (len >= 4 && strcasecmp(entry->d_name + len - 4, ".gif") == 0) ||
            (len >= 4 && strcasecmp(entry->d_name + len - 4, ".png") == 0) ||
            (len >= 4 && strcasecmp(entry->d_name + len - 4, ".jpg") == 0) ||
            (len >= 5 && strcasecmp(entry->d_name + len - 5, ".jpeg") == 0)) {
            file_count++;
        }
    }
    
    if (file_count == 0) {
        ESP_LOGW(TAG, "No animation files found");
        closedir(dir);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Allocate posts array
    ch->posts = calloc(file_count, sizeof(sdcard_post_t));
    if (!ch->posts) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    
    // Second pass: load metadata
    rewinddir(dir);
    size_t idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < file_count) {
        if (entry->d_name[0] == '.') continue;
        
        size_t len = strlen(entry->d_name);
        asset_type_t type;
        // Check longer extensions first (e.g., .jpeg before .jpg), all comparisons are case-insensitive
        if (len >= 5 && strcasecmp(entry->d_name + len - 5, ".webp") == 0) {
            type = ASSET_TYPE_WEBP;
        } else if (len >= 5 && strcasecmp(entry->d_name + len - 5, ".jpeg") == 0) {
            type = ASSET_TYPE_JPEG; // JPEG (prefer .jpg but accept .jpeg)
        } else if (len >= 4 && strcasecmp(entry->d_name + len - 4, ".gif") == 0) {
            type = ASSET_TYPE_GIF;
        } else if (len >= 4 && strcasecmp(entry->d_name + len - 4, ".png") == 0) {
            type = ASSET_TYPE_PNG;
        } else if (len >= 4 && strcasecmp(entry->d_name + len - 4, ".jpg") == 0) {
            type = ASSET_TYPE_JPEG; // JPEG (canonical extension)
        } else {
            continue;
        }
        
        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) continue;
        
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        
        sdcard_post_t *post = &ch->posts[idx];
        post->name = strdup(entry->d_name);
        post->filepath = strdup(full_path);
        post->created_at = st.st_mtime;
        post->type = type;
        post->healthy = true;
        
        if (!post->name || !post->filepath) {
            // Cleanup on failure
            for (size_t i = 0; i <= idx; i++) {
                free(ch->posts[i].name);
                free(ch->posts[i].filepath);
            }
            free(ch->posts);
            ch->posts = NULL;
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        
        idx++;
    }
    closedir(dir);
    
    ch->post_count = idx;
    ch->base.loaded = true;
    
    ESP_LOGI(TAG, "Loaded %zu posts", ch->post_count);
    return ESP_OK;
}

static void sdcard_impl_unload(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return;
    
    if (ch->posts) {
        for (size_t i = 0; i < ch->post_count; i++) {
            free(ch->posts[i].name);
            free(ch->posts[i].filepath);
        }
        free(ch->posts);
        ch->posts = NULL;
    }
    
    free(ch->playback_order);
    ch->playback_order = NULL;
    
    ch->post_count = 0;
    ch->order_count = 0;
    ch->current_pos = 0;
    ch->base.loaded = false;
}

static esp_err_t sdcard_impl_start_playback(channel_handle_t channel, 
                                             channel_order_mode_t order_mode,
                                             const channel_filter_config_t *filter)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->base.loaded) return ESP_ERR_INVALID_STATE;
    
    // Free old order
    free(ch->playback_order);
    ch->playback_order = NULL;
    ch->order_count = 0;
    ch->current_pos = 0;
    
    // Count items that pass filter
    size_t pass_count = 0;
    for (size_t i = 0; i < ch->post_count; i++) {
        if (post_passes_filter(&ch->posts[i], filter)) {
            pass_count++;
        }
    }
    
    if (pass_count == 0) {
        ESP_LOGW(TAG, "No posts pass filter");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Allocate order array
    ch->playback_order = malloc(pass_count * sizeof(uint32_t));
    if (!ch->playback_order) {
        return ESP_ERR_NO_MEM;
    }
    
    // Fill with indices of passing items
    size_t idx = 0;
    for (size_t i = 0; i < ch->post_count && idx < pass_count; i++) {
        if (post_passes_filter(&ch->posts[i], filter)) {
            ch->playback_order[idx++] = (uint32_t)i;
        }
    }
    ch->order_count = pass_count;
    
    // Apply ordering
    ch->base.current_order = order_mode;
    if (filter) {
        ch->base.current_filter = *filter;
    } else {
        memset(&ch->base.current_filter, 0, sizeof(ch->base.current_filter));
    }
    
    switch (order_mode) {
        case CHANNEL_ORDER_CREATED:
            // Sort by date using simple bubble sort (good enough for <1000 items)
            for (size_t i = 0; i < ch->order_count - 1; i++) {
                for (size_t j = 0; j < ch->order_count - i - 1; j++) {
                    time_t ta = ch->posts[ch->playback_order[j]].created_at;
                    time_t tb = ch->posts[ch->playback_order[j+1]].created_at;
                    if (ta < tb) {  // Newest first
                        uint32_t tmp = ch->playback_order[j];
                        ch->playback_order[j] = ch->playback_order[j+1];
                        ch->playback_order[j+1] = tmp;
                    }
                }
            }
            break;
            
        case CHANNEL_ORDER_RANDOM:
            shuffle_order(ch->playback_order, ch->order_count);
            break;
            
        case CHANNEL_ORDER_ORIGINAL:
        default:
            // Keep original (file system) order
            break;
    }
    
    ESP_LOGI(TAG, "Started playback: %zu items, order=%d", ch->order_count, order_mode);
    return ESP_OK;
}

static esp_err_t sdcard_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Advance position
    ch->current_pos++;
    if (ch->current_pos >= ch->order_count) {
        // Wrap around
        ch->current_pos = 0;
        if (ch->base.current_order == CHANNEL_ORDER_RANDOM) {
            shuffle_order(ch->playback_order, ch->order_count);
        }
    }
    
    uint32_t post_idx = ch->playback_order[ch->current_pos];
    fill_item_ref(&ch->posts[post_idx], post_idx, out_item);
    
    return ESP_OK;
}

static esp_err_t sdcard_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Move backward
    if (ch->current_pos == 0) {
        ch->current_pos = ch->order_count - 1;
    } else {
        ch->current_pos--;
    }
    
    uint32_t post_idx = ch->playback_order[ch->current_pos];
    fill_item_ref(&ch->posts[post_idx], post_idx, out_item);
    
    return ESP_OK;
}

static esp_err_t sdcard_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    uint32_t post_idx = ch->playback_order[ch->current_pos];
    fill_item_ref(&ch->posts[post_idx], post_idx, out_item);
    
    return ESP_OK;
}

static esp_err_t sdcard_impl_request_reshuffle(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (ch->base.current_order != CHANNEL_ORDER_RANDOM) {
        return ESP_OK;  // Not in random mode, nothing to do
    }
    
    shuffle_order(ch->playback_order, ch->order_count);
    ch->current_pos = 0;  // Reset to start after shuffle
    
    ESP_LOGI(TAG, "Reshuffled playback order");
    return ESP_OK;
}

static esp_err_t sdcard_impl_request_refresh(channel_handle_t channel)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    // Save current settings
    channel_order_mode_t order = ch->base.current_order;
    channel_filter_config_t filter = ch->base.current_filter;
    
    // Reload
    sdcard_impl_unload(channel);
    esp_err_t err = sdcard_impl_load(channel);
    if (err != ESP_OK) {
        return err;
    }
    
    // Restore playback
    return sdcard_impl_start_playback(channel, order, &filter);
}

static esp_err_t sdcard_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    sdcard_channel_t *ch = (sdcard_channel_t *)channel;
    if (!ch || !out_stats) return ESP_ERR_INVALID_ARG;
    
    out_stats->total_items = ch->post_count;
    out_stats->filtered_items = ch->order_count;
    out_stats->current_position = ch->current_pos;
    
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

// Public creation function

channel_handle_t sdcard_channel_create(const char *name, const char *animations_dir)
{
    sdcard_channel_t *ch = calloc(1, sizeof(sdcard_channel_t));
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

