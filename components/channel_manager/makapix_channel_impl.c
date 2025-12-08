#include "makapix_channel_impl.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "makapix_channel";

/**
 * @brief File extensions enum
 */
typedef enum {
    EXT_WEBP = 0,
    EXT_GIF  = 1,
    EXT_PNG  = 2,
    EXT_JPEG = 3,
} file_extension_t;

/**
 * @brief Internal Makapix channel state
 */
typedef struct {
    struct channel_s base;           // Base channel (must be first)
    
    // Configuration
    char *channel_id;                // UUID of channel
    char *vault_path;                // Base vault path
    char *channels_path;             // Base channels path
    
    // Loaded entries
    makapix_channel_entry_t *entries;  // Array of entries from index.bin
    size_t entry_count;              // Number of entries
    
    // Playback order
    uint32_t *playback_order;        // Indices into entries array
    size_t order_count;              // Items in playback order
    size_t current_pos;              // Current position
    
    // Refresh state
    bool refreshing;                 // Background refresh in progress
    
} makapix_channel_t;

// Forward declarations
static esp_err_t makapix_impl_load(channel_handle_t channel);
static void makapix_impl_unload(channel_handle_t channel);
static esp_err_t makapix_impl_start_playback(channel_handle_t channel, 
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter);
static esp_err_t makapix_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t makapix_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t makapix_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item);
static esp_err_t makapix_impl_request_reshuffle(channel_handle_t channel);
static esp_err_t makapix_impl_request_refresh(channel_handle_t channel);
static esp_err_t makapix_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats);
static void makapix_impl_destroy(channel_handle_t channel);

// Virtual function table
static const channel_ops_t s_makapix_ops = {
    .load = makapix_impl_load,
    .unload = makapix_impl_unload,
    .start_playback = makapix_impl_start_playback,
    .next_item = makapix_impl_next_item,
    .prev_item = makapix_impl_prev_item,
    .current_item = makapix_impl_current_item,
    .request_reshuffle = makapix_impl_request_reshuffle,
    .request_refresh = makapix_impl_request_refresh,
    .get_stats = makapix_impl_get_stats,
    .destroy = makapix_impl_destroy,
};

// Helper: get extension string
static const char *get_ext_string(uint8_t ext)
{
    switch (ext) {
        case EXT_WEBP: return "webp";
        case EXT_GIF:  return "gif";
        case EXT_PNG:  return "png";
        case EXT_JPEG: return "jpg";
        default:       return "webp";
    }
}

// Helper: convert SHA256 to hex string (first 4 bytes only for path)
static void sha256_to_path_prefix(const uint8_t *sha256, char *out, size_t out_len)
{
    // Format: ab/cd (first 4 hex chars split into directories)
    if (out_len >= 6) {
        snprintf(out, out_len, "%02x/%02x", sha256[0], sha256[1]);
    }
}

// Helper: convert SHA256 to full hex string
static void sha256_to_hex(const uint8_t *sha256, char *out, size_t out_len)
{
    if (out_len < 65) return;
    for (int i = 0; i < 32; i++) {
        snprintf(out + i*2, 3, "%02x", sha256[i]);
    }
}

// Helper: build vault filepath for an entry
static void build_vault_path(const makapix_channel_t *ch, 
                              const makapix_channel_entry_t *entry,
                              char *out, size_t out_len)
{
    char prefix[8];
    char hex[65];
    sha256_to_path_prefix(entry->sha256, prefix, sizeof(prefix));
    sha256_to_hex(entry->sha256, hex, sizeof(hex));
    
    snprintf(out, out_len, "%s/%s/%s.%s", 
             ch->vault_path, prefix, hex, get_ext_string(entry->extension));
}

// Helper: get filter flags from entry
static channel_filter_flags_t get_entry_flags(const makapix_channel_entry_t *entry)
{
    channel_filter_flags_t flags = entry->flags;
    
    // Add format flags based on extension
    switch (entry->extension) {
        case EXT_GIF:  flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case EXT_WEBP: flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case EXT_PNG:  flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case EXT_JPEG: flags |= CHANNEL_FILTER_FLAG_JPEG; break;
    }
    
    return flags;
}

// Helper: check if entry passes filter
static bool entry_passes_filter(const makapix_channel_entry_t *entry, 
                                 const channel_filter_config_t *filter)
{
    if (!filter) return true;
    
    channel_filter_flags_t flags = get_entry_flags(entry);
    
    if ((flags & filter->required_flags) != filter->required_flags) {
        return false;
    }
    
    if ((flags & filter->excluded_flags) != 0) {
        return false;
    }
    
    return true;
}

// Helper: fill item ref from entry
static void fill_item_from_entry(const makapix_channel_t *ch,
                                  const makapix_channel_entry_t *entry,
                                  uint32_t index,
                                  channel_item_ref_t *out)
{
    memset(out, 0, sizeof(*out));
    
    build_vault_path(ch, entry, out->filepath, sizeof(out->filepath));
    sha256_to_hex(entry->sha256, out->storage_key, sizeof(out->storage_key));
    out->item_index = index;
    out->flags = get_entry_flags(entry);
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

// Helper: build index.bin path
static void build_index_path(const makapix_channel_t *ch, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s/index.bin", ch->channels_path, ch->channel_id);
}

// Interface implementation

static esp_err_t makapix_impl_load(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    if (ch->base.loaded) {
        ESP_LOGW(TAG, "Channel already loaded");
        makapix_impl_unload(channel);
    }
    
    char index_path[256];
    build_index_path(ch, index_path, sizeof(index_path));
    
    ESP_LOGI(TAG, "Loading channel from: %s", index_path);
    
    // Try to open index file
    FILE *f = fopen(index_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Index file not found: %s (errno=%d)", index_path, errno);
        // Not an error - channel just has no entries yet
        ch->entries = NULL;
        ch->entry_count = 0;
        ch->base.loaded = true;
        return ESP_OK;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size % sizeof(makapix_channel_entry_t) != 0) {
        ESP_LOGE(TAG, "Invalid index file size: %ld", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    size_t entry_count = file_size / sizeof(makapix_channel_entry_t);
    
    // Allocate entries array
    ch->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
    if (!ch->entries) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    // Read entries in chunks to avoid blocking too long
    const size_t CHUNK_SIZE = 100;
    size_t read_count = 0;
    while (read_count < entry_count) {
        size_t to_read = (entry_count - read_count > CHUNK_SIZE) ? 
                          CHUNK_SIZE : (entry_count - read_count);
        size_t read = fread(&ch->entries[read_count], 
                            sizeof(makapix_channel_entry_t), to_read, f);
        if (read != to_read) {
            ESP_LOGE(TAG, "Failed to read index entries");
            free(ch->entries);
            ch->entries = NULL;
            fclose(f);
            return ESP_FAIL;
        }
        read_count += read;
        // TODO: yield to other tasks here in production
    }
    
    fclose(f);
    
    ch->entry_count = entry_count;
    ch->base.loaded = true;
    
    ESP_LOGI(TAG, "Loaded %zu entries", ch->entry_count);
    return ESP_OK;
}

static void makapix_impl_unload(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    
    free(ch->entries);
    ch->entries = NULL;
    
    free(ch->playback_order);
    ch->playback_order = NULL;
    
    ch->entry_count = 0;
    ch->order_count = 0;
    ch->current_pos = 0;
    ch->base.loaded = false;
}

static esp_err_t makapix_impl_start_playback(channel_handle_t channel, 
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->base.loaded) return ESP_ERR_INVALID_STATE;
    
    // Free old order
    free(ch->playback_order);
    ch->playback_order = NULL;
    ch->order_count = 0;
    ch->current_pos = 0;
    
    if (ch->entry_count == 0) {
        ESP_LOGW(TAG, "No entries in channel");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Count entries that pass filter
    size_t pass_count = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (entry_passes_filter(&ch->entries[i], filter)) {
            pass_count++;
        }
    }
    
    if (pass_count == 0) {
        ESP_LOGW(TAG, "No entries pass filter");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Allocate order array
    ch->playback_order = malloc(pass_count * sizeof(uint32_t));
    if (!ch->playback_order) {
        return ESP_ERR_NO_MEM;
    }
    
    // Fill with passing entries
    size_t idx = 0;
    for (size_t i = 0; i < ch->entry_count && idx < pass_count; i++) {
        if (entry_passes_filter(&ch->entries[i], filter)) {
            ch->playback_order[idx++] = (uint32_t)i;
        }
    }
    ch->order_count = pass_count;
    
    // Store settings
    ch->base.current_order = order_mode;
    if (filter) {
        ch->base.current_filter = *filter;
    } else {
        memset(&ch->base.current_filter, 0, sizeof(ch->base.current_filter));
    }
    
    // Apply ordering
    switch (order_mode) {
        case CHANNEL_ORDER_CREATED:
            // Sort by created_at (newest first)
            for (size_t i = 0; i < ch->order_count - 1; i++) {
                for (size_t j = 0; j < ch->order_count - i - 1; j++) {
                    uint32_t ta = ch->entries[ch->playback_order[j]].created_at;
                    uint32_t tb = ch->entries[ch->playback_order[j+1]].created_at;
                    if (ta < tb) {
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
            // Keep order from index.bin (server order)
            break;
    }
    
    ESP_LOGI(TAG, "Started playback: %zu items, order=%d", ch->order_count, order_mode);
    return ESP_OK;
}

static esp_err_t makapix_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    ch->current_pos++;
    if (ch->current_pos >= ch->order_count) {
        ch->current_pos = 0;
        if (ch->base.current_order == CHANNEL_ORDER_RANDOM) {
            shuffle_order(ch->playback_order, ch->order_count);
        }
    }
    
    uint32_t entry_idx = ch->playback_order[ch->current_pos];
    fill_item_from_entry(ch, &ch->entries[entry_idx], entry_idx, out_item);
    
    return ESP_OK;
}

static esp_err_t makapix_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (ch->current_pos == 0) {
        ch->current_pos = ch->order_count - 1;
    } else {
        ch->current_pos--;
    }
    
    uint32_t entry_idx = ch->playback_order[ch->current_pos];
    fill_item_from_entry(ch, &ch->entries[entry_idx], entry_idx, out_item);
    
    return ESP_OK;
}

static esp_err_t makapix_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    uint32_t entry_idx = ch->playback_order[ch->current_pos];
    fill_item_from_entry(ch, &ch->entries[entry_idx], entry_idx, out_item);
    
    return ESP_OK;
}

static esp_err_t makapix_impl_request_reshuffle(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->playback_order || ch->order_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (ch->base.current_order != CHANNEL_ORDER_RANDOM) {
        return ESP_OK;
    }
    
    shuffle_order(ch->playback_order, ch->order_count);
    ch->current_pos = 0;
    
    ESP_LOGI(TAG, "Reshuffled");
    return ESP_OK;
}

static esp_err_t makapix_impl_request_refresh(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return ESP_ERR_INVALID_ARG;
    
    if (ch->refreshing) {
        ESP_LOGW(TAG, "Refresh already in progress");
        return ESP_OK;
    }
    
    // TODO: Implement MQTT-based refresh
    // For now, this is a stub that does nothing
    // In the full implementation, this would:
    // 1. Start an async task to query MQTT server for updates
    // 2. Download new artworks via HTTPS
    // 3. Update index.bin atomically
    // 4. Reload the channel
    
    ESP_LOGW(TAG, "MQTT refresh not yet implemented (stub)");
    return ESP_OK;
}

static esp_err_t makapix_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_stats) return ESP_ERR_INVALID_ARG;
    
    out_stats->total_items = ch->entry_count;
    out_stats->filtered_items = ch->order_count;
    out_stats->current_position = ch->current_pos;
    
    return ESP_OK;
}

static void makapix_impl_destroy(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    
    makapix_impl_unload(channel);
    free(ch->channel_id);
    free(ch->vault_path);
    free(ch->channels_path);
    free(ch->base.name);
    free(ch);
    
    ESP_LOGI(TAG, "Channel destroyed");
}

// Public functions

channel_handle_t makapix_channel_create(const char *channel_id, 
                                         const char *name,
                                         const char *vault_path,
                                         const char *channels_path)
{
    if (!channel_id || !vault_path || !channels_path) {
        ESP_LOGE(TAG, "Invalid parameters");
        return NULL;
    }
    
    makapix_channel_t *ch = calloc(1, sizeof(makapix_channel_t));
    if (!ch) {
        ESP_LOGE(TAG, "Failed to allocate channel");
        return NULL;
    }
    
    ch->base.ops = &s_makapix_ops;
    ch->base.name = name ? strdup(name) : strdup("Makapix");
    ch->channel_id = strdup(channel_id);
    ch->vault_path = strdup(vault_path);
    ch->channels_path = strdup(channels_path);
    ch->base.current_order = CHANNEL_ORDER_ORIGINAL;
    
    if (!ch->base.name || !ch->channel_id || !ch->vault_path || !ch->channels_path) {
        free(ch->base.name);
        free(ch->channel_id);
        free(ch->vault_path);
        free(ch->channels_path);
        free(ch);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Created channel: %s (id=%s)", ch->base.name, ch->channel_id);
    return (channel_handle_t)ch;
}

const char *makapix_channel_get_id(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return ch ? ch->channel_id : NULL;
}

bool makapix_channel_is_refreshing(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    return ch ? ch->refreshing : false;
}

