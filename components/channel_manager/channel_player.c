#include "channel_player.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "channel_player";

// Internal player state
static struct {
    channel_player_state_t state;
    bool initialized;
    
    // Makapix channel support
    channel_player_source_t source_type;
    channel_handle_t makapix_channel;
    sdcard_post_t makapix_current_post;  // Cached current post for API compatibility
    char makapix_filepath_buf[256];      // Buffer for filepath string
    char makapix_name_buf[128];          // Buffer for name string
} s_player = {0};

static void shuffle_indices(size_t *indices, size_t count)
{
    if (count <= 1) {
        return;
    }

    // Fisher-Yates shuffle
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = esp_random() % (i + 1);
        size_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }

    ESP_LOGI(TAG, "Randomized playback order for %zu posts", count);
}

static void free_player_state(void)
{
    if (s_player.state.posts) {
        // Posts are shallow copies from channel, don't free individual fields
        free(s_player.state.posts);
        s_player.state.posts = NULL;
    }
    if (s_player.state.indices) {
        free(s_player.state.indices);
        s_player.state.indices = NULL;
    }
    s_player.state.count = 0;
    s_player.state.current_pos = 0;
}

esp_err_t channel_player_init(void)
{
    if (s_player.initialized) {
        ESP_LOGW(TAG, "Channel player already initialized");
        return ESP_OK;
    }

    memset(&s_player, 0, sizeof(s_player));
    s_player.state.randomize = true; // Default: randomization enabled
    s_player.initialized = true;

    ESP_LOGI(TAG, "Channel player initialized");
    return ESP_OK;
}

void channel_player_deinit(void)
{
    if (!s_player.initialized) {
        return;
    }

    free_player_state();
    s_player.initialized = false;

    ESP_LOGI(TAG, "Channel player deinitialized");
}

esp_err_t channel_player_load_channel(void)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Free existing state
    free_player_state();

    // Get total count
    size_t total_posts = sdcard_channel_get_total_count();
    if (total_posts == 0) {
        ESP_LOGW(TAG, "No posts in channel");
        return ESP_ERR_NOT_FOUND;
    }

    // Determine how many posts to load (up to MAX_POSTS)
    size_t posts_to_load = total_posts > CHANNEL_PLAYER_MAX_POSTS ? 
                          CHANNEL_PLAYER_MAX_POSTS : total_posts;

    ESP_LOGI(TAG, "Loading %zu posts from channel (total: %zu)", posts_to_load, total_posts);

    // Allocate posts array
    s_player.state.posts = (sdcard_post_t *)malloc(posts_to_load * sizeof(sdcard_post_t));
    if (!s_player.state.posts) {
        ESP_LOGE(TAG, "Failed to allocate posts array");
        return ESP_ERR_NO_MEM;
    }

    // Allocate indices array
    s_player.state.indices = (size_t *)malloc(posts_to_load * sizeof(size_t));
    if (!s_player.state.indices) {
        ESP_LOGE(TAG, "Failed to allocate indices array");
        free(s_player.state.posts);
        s_player.state.posts = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Load posts using paginated queries (sorted by date, newest first)
    size_t loaded = 0;
    size_t offset = 0;
    sdcard_query_t query = {
        .sort_order = SDCARD_SORT_BY_DATE,
        .count = SDCARD_CHANNEL_PAGE_SIZE
    };

    while (loaded < posts_to_load) {
        query.offset = offset;
        size_t remaining = posts_to_load - loaded;
        query.count = remaining > SDCARD_CHANNEL_PAGE_SIZE ? 
                      SDCARD_CHANNEL_PAGE_SIZE : remaining;

        sdcard_query_result_t result;
        esp_err_t err = sdcard_channel_query(&query, &result);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to query channel: %s", esp_err_to_name(err));
            free_player_state();
            return err;
        }

        if (result.count == 0) {
            // No more posts available
            break;
        }

        // Copy posts (shallow copy)
        for (size_t i = 0; i < result.count && loaded < posts_to_load; i++) {
            s_player.state.posts[loaded] = result.posts[i];
            s_player.state.indices[loaded] = loaded; // Initial order is sequential
            loaded++;
        }

        // Free query result array (but not the post fields, they're shallow copies)
        free(result.posts);

        offset += result.count;
    }

    s_player.state.count = loaded;

    if (loaded == 0) {
        ESP_LOGW(TAG, "No posts loaded from channel");
        free_player_state();
        return ESP_ERR_NOT_FOUND;
    }

    // Initialize playback order
    if (s_player.state.randomize) {
        shuffle_indices(s_player.state.indices, s_player.state.count);
    } else {
        // Sequential order (already set above)
        for (size_t i = 0; i < s_player.state.count; i++) {
            s_player.state.indices[i] = i;
        }
    }

    s_player.state.current_pos = 0;

    ESP_LOGI(TAG, "Loaded %zu posts for playback (randomize: %s)", 
             s_player.state.count, s_player.state.randomize ? "yes" : "no");

    return ESP_OK;
}

esp_err_t channel_player_get_current_post(const sdcard_post_t **out_post)
{
    if (!s_player.initialized || !out_post) {
        return ESP_ERR_INVALID_ARG;
    }

    // Handle Makapix channel source
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_MAKAPIX && s_player.makapix_channel) {
        channel_item_ref_t item;
        esp_err_t err = channel_current_item(s_player.makapix_channel, &item);
        if (err != ESP_OK) {
            return err;
        }
        
        // Convert channel_item_ref_t to sdcard_post_t
        strncpy(s_player.makapix_filepath_buf, item.filepath, sizeof(s_player.makapix_filepath_buf) - 1);
        s_player.makapix_filepath_buf[sizeof(s_player.makapix_filepath_buf) - 1] = '\0';
        
        // Extract name from filepath (last path component)
        const char *name = strrchr(item.filepath, '/');
        if (name) {
            name++;  // Skip the '/'
        } else {
            name = item.filepath;
        }
        strncpy(s_player.makapix_name_buf, name, sizeof(s_player.makapix_name_buf) - 1);
        s_player.makapix_name_buf[sizeof(s_player.makapix_name_buf) - 1] = '\0';
        
        // Determine asset type from file extension (required - no fallback)
        const char *ext = strrchr(item.filepath, '.');
        if (!ext) {
            ESP_LOGE(TAG, "File has no extension, cannot determine type: %s", item.filepath);
            return ESP_ERR_NOT_SUPPORTED;
        }
        
        asset_type_t type;
        if (strcasecmp(ext, ".webp") == 0) {
            type = ASSET_TYPE_WEBP;
        } else if (strcasecmp(ext, ".gif") == 0) {
            type = ASSET_TYPE_GIF;
        } else if (strcasecmp(ext, ".png") == 0) {
            type = ASSET_TYPE_PNG;
        } else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
            type = ASSET_TYPE_JPEG;
        } else {
            ESP_LOGE(TAG, "Unsupported file extension: %s", ext);
            return ESP_ERR_NOT_SUPPORTED;
        }
        
        s_player.makapix_current_post.name = s_player.makapix_name_buf;
        s_player.makapix_current_post.filepath = s_player.makapix_filepath_buf;
        s_player.makapix_current_post.type = type;
        s_player.makapix_current_post.created_at = 0;  // Not tracked for Makapix
        s_player.makapix_current_post.healthy = true;
        
        *out_post = &s_player.makapix_current_post;
        return ESP_OK;
    }

    // Handle SD card source
    if (s_player.state.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t post_idx = s_player.state.indices[s_player.state.current_pos];
    *out_post = &s_player.state.posts[post_idx];

    return ESP_OK;
}

esp_err_t channel_player_advance(void)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Handle Makapix channel source
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_MAKAPIX && s_player.makapix_channel) {
        channel_item_ref_t item;
        return channel_next_item(s_player.makapix_channel, &item);
    }

    // Handle SD card source
    if (s_player.state.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    s_player.state.current_pos++;

    // Check if we've reached the end
    if (s_player.state.current_pos >= s_player.state.count) {
        // Wrap to beginning
        s_player.state.current_pos = 0;

        // If randomization is enabled, re-randomize
        if (s_player.state.randomize) {
            shuffle_indices(s_player.state.indices, s_player.state.count);
            ESP_LOGD(TAG, "Re-randomized playback order after reaching end");
        }
    }

    return ESP_OK;
}

esp_err_t channel_player_go_back(void)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Handle Makapix channel source
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_MAKAPIX && s_player.makapix_channel) {
        channel_item_ref_t item;
        return channel_prev_item(s_player.makapix_channel, &item);
    }

    // Handle SD card source
    if (s_player.state.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_player.state.current_pos == 0) {
        // Wrap to end
        s_player.state.current_pos = s_player.state.count - 1;
    } else {
        s_player.state.current_pos--;
    }

    return ESP_OK;
}

void channel_player_set_randomize(bool enable)
{
    if (!s_player.initialized) {
        return;
    }

    s_player.state.randomize = enable;

    // If we have posts loaded and enabling randomization, shuffle now
    if (enable && s_player.state.count > 0) {
        shuffle_indices(s_player.state.indices, s_player.state.count);
        s_player.state.current_pos = 0; // Reset to start
        ESP_LOGI(TAG, "Randomization %s, shuffled playback order", enable ? "enabled" : "disabled");
    } else if (!enable && s_player.state.count > 0) {
        // Reset to sequential order
        for (size_t i = 0; i < s_player.state.count; i++) {
            s_player.state.indices[i] = i;
        }
        s_player.state.current_pos = 0;
        ESP_LOGI(TAG, "Randomization disabled, reset to sequential order");
    }
}

bool channel_player_is_randomized(void)
{
    return s_player.initialized && s_player.state.randomize;
}

size_t channel_player_get_current_position(void)
{
    if (!s_player.initialized || s_player.state.count == 0) {
        return SIZE_MAX;
    }

    return s_player.state.current_pos;
}

size_t channel_player_get_post_count(void)
{
    if (!s_player.initialized) {
        return 0;
    }
    
    // Handle Makapix channel source
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_MAKAPIX && s_player.makapix_channel) {
        channel_stats_t stats;
        if (channel_get_stats(s_player.makapix_channel, &stats) == ESP_OK) {
            return stats.filtered_items > 0 ? stats.filtered_items : stats.total_items;
        }
        return 0;
    }
    
    return s_player.state.count;
}

esp_err_t channel_player_switch_to_makapix_channel(channel_handle_t makapix_channel)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!makapix_channel) {
        ESP_LOGE(TAG, "Invalid makapix channel handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    s_player.source_type = CHANNEL_PLAYER_SOURCE_MAKAPIX;
    s_player.makapix_channel = makapix_channel;
    
    ESP_LOGI(TAG, "Switched to Makapix channel as playback source");
    return ESP_OK;
}

esp_err_t channel_player_switch_to_sdcard_channel(void)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    s_player.makapix_channel = NULL;
    
    ESP_LOGI(TAG, "Switched to SD card channel as playback source");
    return ESP_OK;
}

channel_player_source_t channel_player_get_source_type(void)
{
    return s_player.initialized ? s_player.source_type : CHANNEL_PLAYER_SOURCE_SDCARD;
}

