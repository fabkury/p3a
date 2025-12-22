#include "channel_player.h"
#include "sdcard_channel_impl.h"
#include "config_store.h"
#include "swap_future.h"
#include "play_navigator.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "channel_player";

// External: wakes the auto_swap_task in main/p3a_main.c so it can restart its countdown.
extern void auto_swap_reset_timer(void) __attribute__((weak));

// Internal player state
static struct {
    bool initialized;

    channel_player_source_t source_type;
    channel_handle_t current_channel;

    // SD card channel handle (owned by channel_player)
    channel_handle_t sdcard_channel;

    // Cached current post for API compatibility
    sdcard_post_t current_post;
    char filepath_buf[256];
    char name_buf[128];
} s_player = {0};

static channel_order_mode_t map_play_order_to_channel_order(uint8_t play_order)
{
    switch (play_order) {
        case 1: return CHANNEL_ORDER_CREATED;
        case 2: return CHANNEL_ORDER_RANDOM;
        case 0:
        default:
            return CHANNEL_ORDER_ORIGINAL;
    }
}

static esp_err_t ensure_channel_loaded(channel_handle_t ch)
{
    if (!ch) return ESP_ERR_INVALID_STATE;

    esp_err_t err = channel_load(ch);
    if (err != ESP_OK) {
        return err;
    }

    channel_order_mode_t order = map_play_order_to_channel_order(config_store_get_play_order());
    err = channel_start_playback(ch, order, NULL);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t channel_player_init(void)
{
    if (s_player.initialized) {
        ESP_LOGW(TAG, "Channel player already initialized");
        return ESP_OK;
    }

    memset(&s_player, 0, sizeof(s_player));
    s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    s_player.initialized = true;
    return ESP_OK;
}

void channel_player_deinit(void)
{
    if (!s_player.initialized) {
        return;
    }

    if (s_player.sdcard_channel) {
        channel_destroy(s_player.sdcard_channel);
        s_player.sdcard_channel = NULL;
    }

    s_player.current_channel = NULL;
    s_player.initialized = false;
}

esp_err_t channel_player_set_sdcard_channel_handle(channel_handle_t sdcard_channel)
{
    if (!s_player.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Replace existing SD channel handle (owned)
    if (s_player.sdcard_channel && s_player.sdcard_channel != sdcard_channel) {
        channel_destroy(s_player.sdcard_channel);
    }

    s_player.sdcard_channel = sdcard_channel;

    // If currently on SD source, update current channel pointer.
    if (s_player.source_type == CHANNEL_PLAYER_SOURCE_SDCARD) {
        s_player.current_channel = s_player.sdcard_channel;
    }

    return ESP_OK;
}

esp_err_t channel_player_load_channel(void)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_player.current_channel) {
        // Default to SD card channel
        if (!s_player.sdcard_channel) {
            s_player.sdcard_channel = sdcard_channel_create("SD Card", NULL);
            if (!s_player.sdcard_channel) {
                return ESP_ERR_NO_MEM;
            }
        }
        s_player.current_channel = s_player.sdcard_channel;
        s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    }

    // Always refresh channel on load
    channel_request_refresh(s_player.current_channel);
    return ensure_channel_loaded(s_player.current_channel);
}

esp_err_t channel_player_get_current_item(channel_item_ref_t *out_item)
{
    if (!s_player.initialized || !out_item) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }

    return channel_current_item(s_player.current_channel, out_item);
}

esp_err_t channel_player_get_current_post_id(int32_t *out_post_id)
{
    if (!s_player.initialized || !out_post_id) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_post_id = 0;

    if (!s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }

    // Use static buffer to avoid stack allocation in caller's context
    static channel_item_ref_t s_temp_item;
    esp_err_t err = channel_current_item(s_player.current_channel, &s_temp_item);
    if (err == ESP_OK) {
        *out_post_id = s_temp_item.post_id;
    }
    return err;
}

esp_err_t channel_player_get_current_post(const sdcard_post_t **out_post)
{
    if (!s_player.initialized || !out_post) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }

    channel_item_ref_t item;
    esp_err_t err = channel_current_item(s_player.current_channel, &item);
    if (err != ESP_OK) {
        return err;
    }

    // Convert channel_item_ref_t to sdcard_post_t
    strlcpy(s_player.filepath_buf, item.filepath, sizeof(s_player.filepath_buf));

    // Extract name from filepath (last path component)
    const char *name = strrchr(item.filepath, '/');
    name = name ? (name + 1) : item.filepath;
    strlcpy(s_player.name_buf, name, sizeof(s_player.name_buf));

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

    s_player.current_post.name = s_player.name_buf;
    s_player.current_post.filepath = s_player.filepath_buf;
    s_player.current_post.type = type;
    s_player.current_post.created_at = 0;
    s_player.current_post.dwell_time_ms = item.dwell_time_ms;
    s_player.current_post.healthy = true;

    *out_post = &s_player.current_post;
    return ESP_OK;
}

esp_err_t channel_player_advance(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }

    channel_item_ref_t item;
    return channel_next_item(s_player.current_channel, &item);
}

esp_err_t channel_player_go_back(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return ESP_ERR_INVALID_STATE;
    }

    channel_item_ref_t item;
    return channel_prev_item(s_player.current_channel, &item);
}

void channel_player_set_randomize(bool enable)
{
    (void)enable;
    // Deprecated: ordering is managed by play_navigator + per-channel settings.
}

bool channel_player_is_randomized(void)
{
    // Deprecated
    return false;
}

size_t channel_player_get_current_position(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return SIZE_MAX;
    }

    channel_stats_t stats;
    if (channel_get_stats(s_player.current_channel, &stats) != ESP_OK) {
        return SIZE_MAX;
    }

    return stats.current_position;
}

size_t channel_player_get_post_count(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return 0;
    }

    channel_stats_t stats;
    if (channel_get_stats(s_player.current_channel, &stats) != ESP_OK) {
        return 0;
    }

    return stats.total_items;
}

esp_err_t channel_player_switch_to_makapix_channel(channel_handle_t makapix_channel)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!makapix_channel) {
        ESP_LOGE(TAG, "Invalid channel handle");
        return ESP_ERR_INVALID_ARG;
    }

    s_player.source_type = CHANNEL_PLAYER_SOURCE_MAKAPIX;
    s_player.current_channel = makapix_channel;
    if (auto_swap_reset_timer) {
        auto_swap_reset_timer();
    }
    return ESP_OK;
}

esp_err_t channel_player_switch_to_sdcard_channel(void)
{
    if (!s_player.initialized) {
        ESP_LOGE(TAG, "Channel player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_player.sdcard_channel) {
        s_player.sdcard_channel = sdcard_channel_create("SD Card", NULL);
        if (!s_player.sdcard_channel) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_player.source_type = CHANNEL_PLAYER_SOURCE_SDCARD;
    s_player.current_channel = s_player.sdcard_channel;
    if (auto_swap_reset_timer) {
        auto_swap_reset_timer();
    }
    return ESP_OK;
}

channel_player_source_t channel_player_get_source_type(void)
{
    return s_player.initialized ? s_player.source_type : CHANNEL_PLAYER_SOURCE_SDCARD;
}

bool channel_player_is_live_mode_active(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return false;
    }
    
    // Get navigator from current channel and check live_mode flag
    return live_mode_is_active(channel_get_navigator(s_player.current_channel));
}

void *channel_player_get_navigator(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return NULL;
    }
    return channel_get_navigator(s_player.current_channel);
}

void channel_player_exit_live_mode(void)
{
    if (!s_player.initialized || !s_player.current_channel) {
        return;
    }
    
    void *nav = channel_get_navigator(s_player.current_channel);
    if (nav && live_mode_is_active(nav)) {
        ESP_LOGI(TAG, "Manual swap detected - exiting Live Mode");
        live_mode_exit(nav);
    }
}

void channel_player_clear_channel(channel_handle_t channel_to_clear)
{
    if (!s_player.initialized) {
        return;
    }
    
    // Only clear if the current channel matches the one being cleared
    // This prevents race conditions when switching channels
    if (s_player.current_channel == channel_to_clear) {
        ESP_LOGI(TAG, "Clearing current channel pointer (channel about to be destroyed)");
        s_player.current_channel = NULL;
    }
}

esp_err_t channel_player_set_play_order(uint8_t play_order)
{
    if (!s_player.initialized || !s_player.current_channel) {
        ESP_LOGW(TAG, "Cannot set play order: no active channel");
        return ESP_ERR_INVALID_STATE;
    }
    
    play_navigator_t *nav = (play_navigator_t *)channel_get_navigator(s_player.current_channel);
    if (!nav) {
        ESP_LOGW(TAG, "Cannot set play order: no navigator");
        return ESP_ERR_INVALID_STATE;
    }
    
    play_order_mode_t mode;
    switch (play_order) {
        case 1: mode = PLAY_ORDER_CREATED; break;
        case 2: mode = PLAY_ORDER_RANDOM; break;
        case 0:
        default:
            mode = PLAY_ORDER_SERVER;
            break;
    }
    
    ESP_LOGI(TAG, "Hot-swapping play order to %d", (int)mode);
    play_navigator_set_order(nav, mode);
    
    return ESP_OK;
}
