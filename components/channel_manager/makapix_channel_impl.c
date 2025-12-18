#include "makapix_channel_impl.h"
#include "makapix_api.h"
#include "makapix_artwork.h"
#include "play_navigator.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "config_store.h"
#include "channel_settings.h"
#include "sdio_bus.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

// TEMP DEBUG: `statvfs()` is not available in this ESP-IDF/newlib configuration.
// Keep the free-space report disabled (see makapix_temp_debug_log_rename_failure()).
#define MAKAPIX_HAVE_STATVFS 0

static const char *TAG = "makapix_channel";

// TEMP DEBUG: Instrument rename() failures for channel index atomic writes.
// Remove this block (or set to 0) once the root cause is understood.
#define MAKAPIX_TEMP_DEBUG_RENAME_FAIL 1

#if MAKAPIX_TEMP_DEBUG_RENAME_FAIL
static void makapix_temp_debug_log_rename_failure(const char *src_path, const char *dst_path, int rename_errno)
{
    if (!src_path || !dst_path) return;

    // Capture high-signal context first
    ESP_LOGE(TAG,
             "TEMP DEBUG: rename('%s' -> '%s') failed: errno=%d (%s), task='%s', core=%d, uptime_us=%lld",
             src_path,
             dst_path,
             rename_errno,
             strerror(rename_errno),
             pcTaskGetName(NULL),
             xPortGetCoreID(),
             (long long)esp_timer_get_time());

    // Stat source (temp) file
    struct stat st_src = {0};
    if (stat(src_path, &st_src) == 0) {
        ESP_LOGE(TAG,
                 "TEMP DEBUG: src stat ok: mode=0%o size=%ld mtime=%ld",
                 (unsigned int)st_src.st_mode,
                 (long)st_src.st_size,
                 (long)st_src.st_mtime);
    } else {
        int e = errno;
        ESP_LOGE(TAG, "TEMP DEBUG: src stat failed: errno=%d (%s)", e, strerror(e));
    }

    // Stat destination (final) file
    struct stat st_dst = {0};
    if (stat(dst_path, &st_dst) == 0) {
        ESP_LOGE(TAG,
                 "TEMP DEBUG: dst stat ok (dst exists): mode=0%o size=%ld mtime=%ld",
                 (unsigned int)st_dst.st_mode,
                 (long)st_dst.st_size,
                 (long)st_dst.st_mtime);
    } else {
        int e = errno;
        ESP_LOGE(TAG, "TEMP DEBUG: dst stat failed (dst likely missing): errno=%d (%s)", e, strerror(e));
    }

    // Try to report filesystem free space at/near destination (best-effort).
    // (FATFS often returns EEXIST if dst already exists and overwrite-on-rename is not supported)
#if MAKAPIX_HAVE_STATVFS
    struct statvfs vfs = {0};
    if (statvfs(dst_path, &vfs) == 0) {
        unsigned long long bsize = (unsigned long long)vfs.f_frsize ? (unsigned long long)vfs.f_frsize
                                                                    : (unsigned long long)vfs.f_bsize;
        unsigned long long free_bytes = bsize * (unsigned long long)vfs.f_bavail;
        unsigned long long total_bytes = bsize * (unsigned long long)vfs.f_blocks;
        ESP_LOGE(TAG,
                 "TEMP DEBUG: statvfs ok: bsize=%llu blocks=%llu bavail=%llu => free=%llu bytes, total=%llu bytes",
                 bsize,
                 (unsigned long long)vfs.f_blocks,
                 (unsigned long long)vfs.f_bavail,
                 free_bytes,
                 total_bytes);
    } else {
        int e = errno;
        ESP_LOGE(TAG, "TEMP DEBUG: statvfs failed: errno=%d (%s)", e, strerror(e));
    }
#else
    ESP_LOGE(TAG, "TEMP DEBUG: statvfs unavailable in this build; skipping free-space report");
#endif
}
#endif

// Cheap validation for index files. This is intentionally lightweight (no CRC/header).
// It prevents promoting/loading clearly corrupt files (e.g., truncated writes).
// NOTE: entry size is fixed at 64 bytes (see makapix_channel_entry_t).
#define MAKAPIX_INDEX_MAX_ENTRIES 8192  // 8192 * 64 = 512KB max index size

static bool makapix_index_file_stat_is_valid(const struct stat *st)
{
    if (!st) return false;
    if (!S_ISREG(st->st_mode)) return false;
    if (st->st_size <= 0) return false;
    if ((st->st_size % (long)sizeof(makapix_channel_entry_t)) != 0) return false;
    if (st->st_size > (long)(MAKAPIX_INDEX_MAX_ENTRIES * sizeof(makapix_channel_entry_t))) return false;
    return true;
}

// Recover/cleanup channel index (.bin) and its temp (.bin.tmp) before any loads.
// This addresses power-loss/crash windows and FATFS rename semantics.
// TEMP NOTE: This is not debug-only; it is required correctness on filesystems where rename() won't overwrite.
static void makapix_index_recover_and_cleanup(const char *index_path)
{
    if (!index_path) return;

    char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", index_path);

    struct stat st_bin = {0};
    struct stat st_tmp = {0};
    const bool have_bin = (stat(index_path, &st_bin) == 0);
    const bool have_tmp = (stat(tmp_path, &st_tmp) == 0);

    if (!have_tmp) return;

    const bool tmp_valid = makapix_index_file_stat_is_valid(&st_tmp);
    const bool bin_valid = have_bin ? makapix_index_file_stat_is_valid(&st_bin) : false;

    if (!tmp_valid) {
        ESP_LOGW(TAG, "Index temp file invalid; removing: %s (size=%ld)", tmp_path, (long)st_tmp.st_size);
        unlink(tmp_path);
        return;
    }

    if (!have_bin) {
        // Index missing but temp exists and looks valid -> promote temp.
        ESP_LOGW(TAG, "Recovering missing channel index from temp: %s -> %s", tmp_path, index_path);
        if (rename(tmp_path, index_path) != 0) {
            ESP_LOGE(TAG, "Failed to recover index from temp: errno=%d (%s)", errno, strerror(errno));
        }
        return;
    }

    // Both exist. Choose which to keep.
    // If the existing index is invalid, or temp is newer-or-equal, promote temp.
    if (!bin_valid || st_tmp.st_mtime >= st_bin.st_mtime) {
        ESP_LOGW(TAG, "Promoting index temp over existing channel index (bin_valid=%d): %s -> %s",
                 (int)bin_valid, tmp_path, index_path);
        (void)unlink(index_path); // required on FATFS: rename() won't overwrite
        if (rename(tmp_path, index_path) != 0) {
            ESP_LOGE(TAG, "Failed to promote index temp: errno=%d (%s)", errno, strerror(errno));
        }
        return;
    }

    // Existing index appears valid and newer than temp -> discard temp.
    ESP_LOGW(TAG, "Discarding stale index temp file: %s", tmp_path);
    unlink(tmp_path);
}

static uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                           uint32_t channel_override_ms,
                                           uint32_t playlist_or_artwork_ms);

// Weak symbol for SD pause check - prevents downloads during OTA to avoid SDIO bus contention
extern bool animation_player_is_sd_paused(void) __attribute__((weak));

/**
 * @brief File extensions enum
 */
typedef enum {
    EXT_WEBP = 0,
    EXT_GIF  = 1,
    EXT_PNG  = 2,
    EXT_JPEG = 3,
} file_extension_t;

// Extension strings for building file paths
static const char *s_ext_strings[] = { ".webp", ".gif", ".png", ".jpg" };

// Best-effort ISO8601 UTC parser: "YYYY-MM-DDTHH:MM:SSZ" -> time_t.
// Returns 0 on failure.
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
    makapix_channel_entry_t *entries;  // Array of entries from channel index file (<channel>.bin)
    size_t entry_count;              // Number of entries
    
    // Playback (playlist-aware)
    play_navigator_t navigator;
    bool navigator_ready;
    uint32_t channel_dwell_override_ms;
    
    // Refresh state
    bool refreshing;                 // Background refresh in progress
    TaskHandle_t refresh_task;      // Background refresh task handle
    time_t last_refresh_time;        // Last successful refresh timestamp

    // Serialize channel index load/write to avoid races during unlink+rename window
    SemaphoreHandle_t index_io_lock;
    
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
static void refresh_task_impl(void *pvParameters);
static file_extension_t detect_file_type(const char *url);
static esp_err_t save_channel_metadata(makapix_channel_t *ch, const char *cursor, time_t refresh_time);
static esp_err_t load_channel_metadata(makapix_channel_t *ch, char *out_cursor, time_t *out_refresh_time);
static esp_err_t update_index_bin(makapix_channel_t *ch, const makapix_post_t *posts, size_t count);
static esp_err_t evict_excess_artworks(makapix_channel_t *ch, size_t max_count);
static void queue_downloads_by_play_position(makapix_channel_t *ch, size_t max_to_queue);
static void build_vault_path_from_storage_key(const makapix_channel_t *ch, const char *storage_key, 
                                               file_extension_t ext, char *out, size_t out_len);
static size_t makapix_impl_get_post_count(channel_handle_t channel);
static esp_err_t makapix_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post);
static void *makapix_impl_get_navigator(channel_handle_t channel);

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
    .get_post_count = makapix_impl_get_post_count,
    .get_post = makapix_impl_get_post,
    .get_navigator = makapix_impl_get_navigator,
};


// Helper: parse UUID string to 16 bytes (removes hyphens)
// Input: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars) or "xxxxxxxx..." (32 chars)
// Output: 16 bytes
static bool uuid_to_bytes(const char *uuid_str, uint8_t *out_bytes)
{
    if (!uuid_str || !out_bytes) return false;
    
    size_t len = strlen(uuid_str);
    int out_idx = 0;
    
    for (size_t i = 0; i < len && out_idx < 16; i++) {
        if (uuid_str[i] == '-') continue;  // Skip hyphens
        
        if (i + 1 >= len) return false;
        
        unsigned int byte;
        char hex[3] = { uuid_str[i], uuid_str[i+1], '\0' };
        if (sscanf(hex, "%02x", &byte) != 1) return false;
        
        out_bytes[out_idx++] = (uint8_t)byte;
        i++;  // Skip second hex char (loop will increment again)
    }
    
    return (out_idx == 16);
}

// Helper: convert 16 bytes back to UUID string with hyphens
// Output format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars + null)
static void bytes_to_uuid(const uint8_t *bytes, char *out, size_t out_len)
{
    if (!bytes || !out || out_len < 37) return;
    
    snprintf(out, out_len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

// Helper: simple hash function for storage_key
static uint32_t hash_string(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

// Helper: comparison function for qsort (entries by created_at, oldest first)
static int compare_entries_by_created(const void *a, const void *b)
{
    const makapix_channel_entry_t *ea = (const makapix_channel_entry_t *)a;
    const makapix_channel_entry_t *eb = (const makapix_channel_entry_t *)b;
    if (ea->created_at < eb->created_at) return -1;
    if (ea->created_at > eb->created_at) return 1;
    return 0;
}

// Helper: build vault filepath for an artwork entry
// NOTE: Must match the pattern used by makapix_artwork_download() in makapix_artwork.c
// Path: /vault/{dir1}/{dir2}/{storage_key}.{ext}
// where dir1/dir2 are derived from hash_string(storage_key)
// storage_key is a UUID stored as 16 bytes in entry->storage_key_uuid
static void build_vault_path(const makapix_channel_t *ch, 
                              const makapix_channel_entry_t *entry,
                              char *out, size_t out_len)
{
    // Convert stored bytes back to UUID string
    char storage_key[40];
    bytes_to_uuid(entry->storage_key_uuid, storage_key, sizeof(storage_key));
    
    // Use same hash function as makapix_artwork_download
    uint32_t hash = hash_string(storage_key);
    char dir1[3], dir2[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)((hash >> 24) & 0xFF));
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)((hash >> 16) & 0xFF));
    
    // Include file extension for type detection
    int ext_idx = (entry->extension <= EXT_JPEG) ? entry->extension : EXT_WEBP;
    snprintf(out, out_len, "%s/%s/%s/%s%s", 
             ch->vault_path, dir1, dir2, storage_key, s_ext_strings[ext_idx]);
}

// Helper: get filter flags from entry
static channel_filter_flags_t get_entry_flags(const makapix_channel_entry_t *entry)
{
    channel_filter_flags_t flags = entry->filter_flags;
    
    // Add format flags based on extension (artwork posts only)
    if (entry->kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
        switch (entry->extension) {
            case EXT_GIF:  flags |= CHANNEL_FILTER_FLAG_GIF; break;
            case EXT_WEBP: flags |= CHANNEL_FILTER_FLAG_WEBP; break;
            case EXT_PNG:  flags |= CHANNEL_FILTER_FLAG_PNG; break;
            case EXT_JPEG: flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        }
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
    bytes_to_uuid(entry->storage_key_uuid, out->storage_key, sizeof(out->storage_key));
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

// Helper: build channel index path (new layout: /sdcard/channel/<channel>.bin)
static void build_index_path(const makapix_channel_t *ch, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s.bin", ch->channels_path, ch->channel_id);
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

    // Recover/cleanup channel index (.bin) and temp (.bin.tmp) before attempting to load.
    // This is required because FATFS rename() won't overwrite an existing destination (EEXIST).
    if (ch->index_io_lock) {
        xSemaphoreTake(ch->index_io_lock, portMAX_DELAY);
    }
    makapix_index_recover_and_cleanup(index_path);
    
    ESP_LOGI(TAG, "Loading channel from: %s", index_path);
    
    // Try to open index file
    FILE *f = fopen(index_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Index file not found: %s (errno=%d)", index_path, errno);
        // Not an error - channel just has no entries yet
        ch->entries = NULL;
        ch->entry_count = 0;
        ch->base.loaded = true;
        
        // Start refresh to fetch channel data from server
        if (!ch->refreshing) {
            ESP_LOGI(TAG, "Starting refresh to populate empty channel");
            esp_err_t refresh_err = makapix_impl_request_refresh(channel);
            if (refresh_err != ESP_OK) {
                // Refresh task failed to start - caller needs to know
                ESP_LOGE(TAG, "Failed to start refresh for empty channel");
                if (ch->index_io_lock) {
                    xSemaphoreGive(ch->index_io_lock);
                }
                return refresh_err;
            }
        }
        if (ch->index_io_lock) {
            xSemaphoreGive(ch->index_io_lock);
        }
        return ESP_OK;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size % sizeof(makapix_channel_entry_t) != 0) {
        // Likely stale/legacy index format. Delete and repopulate.
        ESP_LOGW(TAG, "Invalid/stale channel index size (%ld). Deleting and refreshing: %s", file_size, index_path);
        fclose(f);
        unlink(index_path);
        ch->entries = NULL;
        ch->entry_count = 0;
        ch->base.loaded = true;
        if (!ch->refreshing) {
            if (ch->index_io_lock) {
                xSemaphoreGive(ch->index_io_lock);
            }
            return makapix_impl_request_refresh(channel);
        }
        if (ch->index_io_lock) {
            xSemaphoreGive(ch->index_io_lock);
        }
        return ESP_OK;
    }
    
    size_t entry_count = file_size / sizeof(makapix_channel_entry_t);
    
    // Allocate entries array
    ch->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
    if (!ch->entries) {
        fclose(f);
        if (ch->index_io_lock) {
            xSemaphoreGive(ch->index_io_lock);
        }
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
            if (ch->index_io_lock) {
                xSemaphoreGive(ch->index_io_lock);
            }
            return ESP_FAIL;
        }
        read_count += read;
        // TODO: yield to other tasks here in production
    }
    
    fclose(f);
    
    ch->entry_count = entry_count;
    ch->base.loaded = true;
    
    ESP_LOGI(TAG, "Loaded %zu entries", ch->entry_count);
    
    // Start refresh if not already refreshing
    if (!ch->refreshing) {
        makapix_impl_request_refresh(channel);
    }

    if (ch->index_io_lock) {
        xSemaphoreGive(ch->index_io_lock);
    }
    
    return ESP_OK;
}

static void makapix_impl_unload(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    
    if (ch->navigator_ready) {
        play_navigator_deinit(&ch->navigator);
        ch->navigator_ready = false;
    }

    free(ch->entries);
    ch->entries = NULL;

    ch->entry_count = 0;
    ch->base.loaded = false;
}

static esp_err_t makapix_impl_start_playback(channel_handle_t channel, 
                                              channel_order_mode_t order_mode,
                                              const channel_filter_config_t *filter)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->base.loaded) return ESP_ERR_INVALID_STATE;

    // Store settings
    ch->base.current_order = order_mode;
    if (filter) {
        ch->base.current_filter = *filter;
    } else {
        memset(&ch->base.current_filter, 0, sizeof(ch->base.current_filter));
    }

    // (Re)initialize playlist-aware navigator for item playback
    if (ch->navigator_ready) {
        play_navigator_deinit(&ch->navigator);
        ch->navigator_ready = false;
    }

    play_order_mode_t play_order = PLAY_ORDER_SERVER;
    switch (order_mode) {
        case CHANNEL_ORDER_CREATED: play_order = PLAY_ORDER_CREATED; break;
        case CHANNEL_ORDER_RANDOM:  play_order = PLAY_ORDER_RANDOM;  break;
        case CHANNEL_ORDER_ORIGINAL:
        default:                   play_order = PLAY_ORDER_SERVER;  break;
    }

    uint32_t pe = config_store_get_pe();
    channel_settings_t settings = {0};
    if (channel_settings_load_for_channel_id(ch->channel_id, &settings) != ESP_OK) {
        // not fatal: fall back to global config_store values
        memset(&settings, 0, sizeof(settings));
    }

    if (settings.pe_present) {
        pe = settings.pe;
    }

    if (settings.channel_dwell_time_present) {
        ch->channel_dwell_override_ms = settings.channel_dwell_time_ms;
    } else {
        ch->channel_dwell_override_ms = 0;
    }

    uint32_t global_seed = config_store_get_global_seed();

    esp_err_t err = play_navigator_init(&ch->navigator, channel, ch->channel_id, play_order, pe, global_seed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init play navigator: %s", esp_err_to_name(err));
        return err;
    }

    play_navigator_set_channel_dwell_override_ms(&ch->navigator, ch->channel_dwell_override_ms);

    // Apply per-channel settings, falling back to global config when absent.
    play_navigator_set_randomize_playlist(&ch->navigator,
                                         settings.randomize_playlist_present ? settings.randomize_playlist
                                                                             : config_store_get_randomize_playlist());
    play_navigator_set_live_mode(&ch->navigator,
                                 settings.live_mode_present ? settings.live_mode
                                                           : config_store_get_live_mode());

    ch->navigator_ready = true;
    ESP_LOGI(TAG, "Started playback (navigator): posts=%zu order=%d pe=%lu",
             channel_get_post_count(channel), order_mode, (unsigned long)pe);
    return ESP_OK;
}

static esp_err_t makapix_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_item || !ch->navigator_ready) return ESP_ERR_NOT_FOUND;

    // Track position before navigation
    uint32_t old_p = ch->navigator.p;
    
    artwork_ref_t art = {0};
    esp_err_t err = play_navigator_next(&ch->navigator, &art);
    if (err != ESP_OK) return err;

    memset(out_item, 0, sizeof(*out_item));
    strlcpy(out_item->filepath, art.filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art.storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;
    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art.dwell_time_ms);
    switch (art.type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }
    
    // If position wrapped around or changed significantly, re-queue downloads
    // to ensure we're downloading ahead of the current position
    uint32_t new_p = ch->navigator.p;
    if (new_p < old_p || (new_p - old_p) > 10) {
        // Position wrapped or jumped - re-prioritize downloads
        download_manager_cancel_channel(ch->channel_id);
        queue_downloads_by_play_position(ch, 32);
    }
    
    return ESP_OK;
}

static esp_err_t makapix_impl_prev_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_item || !ch->navigator_ready) return ESP_ERR_NOT_FOUND;

    // Track position before navigation
    uint32_t old_p = ch->navigator.p;
    
    artwork_ref_t art = {0};
    esp_err_t err = play_navigator_prev(&ch->navigator, &art);
    if (err != ESP_OK) return err;

    memset(out_item, 0, sizeof(*out_item));
    strlcpy(out_item->filepath, art.filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art.storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;
    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art.dwell_time_ms);
    switch (art.type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }
    
    // If position wrapped around or changed significantly, re-queue downloads
    // to ensure we're downloading ahead of the current position
    uint32_t new_p = ch->navigator.p;
    if (new_p > old_p || (old_p - new_p) > 10) {
        // Position wrapped backwards or jumped - re-prioritize downloads
        download_manager_cancel_channel(ch->channel_id);
        queue_downloads_by_play_position(ch, 32);
    }
    
    return ESP_OK;
}

static esp_err_t makapix_impl_current_item(channel_handle_t channel, channel_item_ref_t *out_item)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_item || !ch->navigator_ready) return ESP_ERR_NOT_FOUND;

    artwork_ref_t art = {0};
    esp_err_t err = play_navigator_current(&ch->navigator, &art);
    if (err != ESP_OK) return err;

    memset(out_item, 0, sizeof(*out_item));
    strlcpy(out_item->filepath, art.filepath, sizeof(out_item->filepath));
    strlcpy(out_item->storage_key, art.storage_key, sizeof(out_item->storage_key));
    out_item->item_index = 0;
    out_item->flags = CHANNEL_FILTER_FLAG_NONE;
    out_item->dwell_time_ms = compute_effective_dwell_ms(config_store_get_dwell_time(),
                                                         ch->channel_dwell_override_ms,
                                                         art.dwell_time_ms);
    switch (art.type) {
        case ASSET_TYPE_GIF:  out_item->flags |= CHANNEL_FILTER_FLAG_GIF; break;
        case ASSET_TYPE_WEBP: out_item->flags |= CHANNEL_FILTER_FLAG_WEBP; break;
        case ASSET_TYPE_PNG:  out_item->flags |= CHANNEL_FILTER_FLAG_PNG; break;
        case ASSET_TYPE_JPEG: out_item->flags |= CHANNEL_FILTER_FLAG_JPEG; break;
        default: break;
    }
    return ESP_OK;
}

static esp_err_t makapix_impl_request_reshuffle(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !ch->navigator_ready) return ESP_ERR_INVALID_STATE;
    if (ch->base.current_order != CHANNEL_ORDER_RANDOM) return ESP_OK;
    // Deterministic shuffle is derived from global_seed; just rebuild navigator order.
    play_navigator_set_order(&ch->navigator, PLAY_ORDER_RANDOM);
    ESP_LOGI(TAG, "Reshuffled (navigator)");
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
    
    // Start background refresh task
    // Stack needs to be large enough for MQTT+TLS, HTTP downloads, JSON parsing
    // Note: makapix_query_response_t is allocated on heap in refresh task
    ch->refreshing = true;
    BaseType_t ret = xTaskCreate(refresh_task_impl, "makapix_refresh", 24576, ch, 5, &ch->refresh_task);
    if (ret != pdPASS) {
        ch->refreshing = false;
        ESP_LOGE(TAG, "Failed to create refresh task");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Refresh task started for channel %s", ch->channel_id);
    return ESP_OK;
}

static esp_err_t makapix_impl_get_stats(channel_handle_t channel, channel_stats_t *out_stats)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_stats) return ESP_ERR_INVALID_ARG;
    
    out_stats->total_items = ch->entry_count;
    out_stats->filtered_items = ch->entry_count;
    if (ch->navigator_ready) {
        uint32_t p = 0, q = 0;
        play_navigator_get_position(&ch->navigator, &p, &q);
        (void)q;
        out_stats->current_position = p;
    } else {
        out_stats->current_position = 0;
    }
    
    return ESP_OK;
}

static size_t makapix_impl_get_post_count(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return 0;
    return ch->entry_count;
}

static uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                           uint32_t channel_override_ms,
                                           uint32_t playlist_or_artwork_ms)
{
    uint32_t eff = playlist_or_artwork_ms;
    if (channel_override_ms != 0) eff = channel_override_ms;
    if (global_override_ms != 0) eff = global_override_ms;
    if (eff == 0) eff = 30000;
    return eff;
}

// Forward declaration (used by item helpers below)
static uint32_t compute_effective_dwell_ms(uint32_t global_override_ms,
                                           uint32_t channel_override_ms,
                                           uint32_t playlist_or_artwork_ms);

static esp_err_t makapix_impl_get_post(channel_handle_t channel, size_t post_index, channel_post_t *out_post)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch || !out_post) return ESP_ERR_INVALID_ARG;
    if (!ch->base.loaded) return ESP_ERR_INVALID_STATE;
    if (post_index >= ch->entry_count) return ESP_ERR_INVALID_ARG;

    const makapix_channel_entry_t *entry = &ch->entries[post_index];
    memset(out_post, 0, sizeof(*out_post));

    out_post->post_id = entry->post_id;
    out_post->kind = (entry->kind == MAKAPIX_INDEX_POST_KIND_PLAYLIST) ? CHANNEL_POST_KIND_PLAYLIST : CHANNEL_POST_KIND_ARTWORK;
    out_post->created_at = entry->created_at;
    out_post->metadata_modified_at = (time_t)entry->metadata_modified_at;
    out_post->dwell_time_ms = entry->dwell_time_ms; // 0 = channel default

    if (out_post->kind == CHANNEL_POST_KIND_PLAYLIST) {
        out_post->u.playlist.total_artworks = entry->total_artworks;
    } else {
        // Fill artwork fields
        build_vault_path(ch, entry, out_post->u.artwork.filepath, sizeof(out_post->u.artwork.filepath));
        bytes_to_uuid(entry->storage_key_uuid, out_post->u.artwork.storage_key, sizeof(out_post->u.artwork.storage_key));
        out_post->u.artwork.art_url[0] = '\0';

        switch (entry->extension) {
            case EXT_WEBP: out_post->u.artwork.type = ASSET_TYPE_WEBP; break;
            case EXT_GIF:  out_post->u.artwork.type = ASSET_TYPE_GIF;  break;
            case EXT_PNG:  out_post->u.artwork.type = ASSET_TYPE_PNG;  break;
            case EXT_JPEG: out_post->u.artwork.type = ASSET_TYPE_JPEG; break;
            default:       out_post->u.artwork.type = ASSET_TYPE_WEBP; break;
        }

        out_post->u.artwork.width = 0;
        out_post->u.artwork.height = 0;
        out_post->u.artwork.frame_count = 0;
        out_post->u.artwork.has_transparency = false;
        out_post->u.artwork.artwork_modified_at = (time_t)entry->artwork_modified_at;
    }

    return ESP_OK;
}

// Helper: detect file type from URL (case-insensitive, accepts both .jpg and .jpeg)
static file_extension_t detect_file_type(const char *url)
{
    if (!url) return EXT_WEBP;
    size_t len = strlen(url);
    // Check longer extensions first (e.g., .jpeg before .jpg)
    if (len >= 5 && strcasecmp(url + len - 5, ".webp") == 0) return EXT_WEBP;
    if (len >= 5 && strcasecmp(url + len - 5, ".jpeg") == 0) return EXT_JPEG; // JPEG (prefer .jpg but accept .jpeg)
    if (len >= 4 && strcasecmp(url + len - 4, ".gif") == 0) return EXT_GIF;
    if (len >= 4 && strcasecmp(url + len - 4, ".png") == 0) return EXT_PNG;
    if (len >= 4 && strcasecmp(url + len - 4, ".jpg") == 0) return EXT_JPEG; // JPEG (canonical extension)
    return EXT_WEBP;
}

// Helper: save channel metadata JSON
static esp_err_t save_channel_metadata(makapix_channel_t *ch, const char *cursor, time_t refresh_time)
{
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", ch->channels_path, ch->channel_id);
    
    cJSON *meta = cJSON_CreateObject();
    if (!meta) return ESP_ERR_NO_MEM;
    
    if (cursor && strlen(cursor) > 0) {
        cJSON_AddStringToObject(meta, "cursor", cursor);
    } else {
        cJSON_AddNullToObject(meta, "cursor");
    }
    cJSON_AddNumberToObject(meta, "last_refresh", (double)refresh_time);
    
    char *json_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!json_str) return ESP_ERR_NO_MEM;
    
    // Atomic write: write to temp file, then rename (meta_path is 256, need 4 more for .tmp)
    char temp_path[260];
    size_t path_len = strlen(meta_path);
    if (path_len + 4 < sizeof(temp_path)) {
        snprintf(temp_path, sizeof(temp_path), "%s.tmp", meta_path);
    } else {
        ESP_LOGE(TAG, "Meta path too long for temp file");
        free(json_str);
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *f = fopen(temp_path, "w");
    if (!f) {
        free(json_str);
        return ESP_FAIL;
    }
    
    fprintf(f, "%s", json_str);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(json_str);
    
    // Rename temp to final
    if (rename(temp_path, meta_path) != 0) {
        unlink(temp_path);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Helper: load channel metadata JSON
static esp_err_t load_channel_metadata(makapix_channel_t *ch, char *out_cursor, time_t *out_refresh_time)
{
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/%s.json", ch->channels_path, ch->channel_id);
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", meta_path);
    struct stat tmp_st;
    if (stat(tmp_path, &tmp_st) == 0 && S_ISREG(tmp_st.st_mode)) {
        ESP_LOGD(TAG, "Removing orphan temp file: %s", tmp_path);
        unlink(tmp_path);
    }
    
    FILE *f = fopen(meta_path, "r");
    if (!f) {
        if (out_cursor) out_cursor[0] = '\0';
        if (out_refresh_time) *out_refresh_time = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 4096) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    char *json_buf = malloc(size + 1);
    if (!json_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    fread(json_buf, 1, size, f);
    json_buf[size] = '\0';
    fclose(f);
    
    cJSON *meta = cJSON_Parse(json_buf);
    free(json_buf);
    if (!meta) return ESP_ERR_INVALID_RESPONSE;
    
    cJSON *cursor = cJSON_GetObjectItem(meta, "cursor");
    if (out_cursor) {
        if (cJSON_IsString(cursor)) {
            strncpy(out_cursor, cursor->valuestring, 63);
            out_cursor[63] = '\0';
        } else {
            out_cursor[0] = '\0';
        }
    }
    
    cJSON *refresh = cJSON_GetObjectItem(meta, "last_refresh");
    if (out_refresh_time) {
        *out_refresh_time = cJSON_IsNumber(refresh) ? (time_t)cJSON_GetNumberValue(refresh) : 0;
    }
    
    cJSON_Delete(meta);
    return ESP_OK;
}

// Helper: update channel index (.bin) with new posts
static esp_err_t update_index_bin(makapix_channel_t *ch, const makapix_post_t *posts, size_t count)
{
    if (!ch || !posts) return ESP_ERR_INVALID_ARG;

    char index_path[256];
    build_index_path(ch, index_path, sizeof(index_path));

    const bool have_lock = (ch->index_io_lock != NULL);
    if (have_lock) {
        xSemaphoreTake(ch->index_io_lock, portMAX_DELAY);
    }
    esp_err_t ret = ESP_OK;

    // Ensure directory exists - create recursively
    char dir_path[256];
    strncpy(dir_path, index_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    char *dir_sep = strrchr(dir_path, '/');
    if (dir_sep) {
        *dir_sep = '\0';
        for (char *p = dir_path + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                struct stat st;
                if (stat(dir_path, &st) != 0) {
                    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                        ESP_LOGE(TAG, "Failed to create directory %s: %d", dir_path, errno);
                    }
                }
                *p = '/';
            }
        }
        struct stat st;
        if (stat(dir_path, &st) != 0) {
            if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "Failed to create directory %s: %d", dir_path, errno);
            }
        }
    }

    // Copy existing entries
    makapix_channel_entry_t *all_entries = NULL;
    size_t all_count = ch->entry_count;
    if (ch->entries && ch->entry_count > 0) {
        all_entries = malloc((all_count + count) * sizeof(makapix_channel_entry_t));
        if (!all_entries) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
        memcpy(all_entries, ch->entries, all_count * sizeof(makapix_channel_entry_t));
    } else {
        all_entries = malloc(count * sizeof(makapix_channel_entry_t));
        if (!all_entries) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
        all_count = 0;
    }

    for (size_t i = 0; i < count; i++) {
        const makapix_post_t *post = &posts[i];

        // Find existing entry by (post_id, kind)
        int found_idx = -1;
        for (size_t j = 0; j < all_count; j++) {
            if (all_entries[j].post_id == post->post_id && all_entries[j].kind == (uint8_t)post->kind) {
                found_idx = (int)j;
                break;
            }
        }

        makapix_channel_entry_t tmp = {0};
        tmp.post_id = post->post_id;
        tmp.kind = (uint8_t)post->kind;
        tmp.created_at = (uint32_t)parse_iso8601_utc(post->created_at);
        tmp.metadata_modified_at = (uint32_t)parse_iso8601_utc(post->metadata_modified_at);
        tmp.filter_flags = 0;

        if (post->kind == MAKAPIX_POST_KIND_ARTWORK) {
            // Just add the entry to the index - downloads happen separately via download_manager
            // The artwork will be played when its file becomes available (file_exists check in navigator)
            uint8_t uuid_bytes[16] = {0};
            if (!uuid_to_bytes(post->storage_key, uuid_bytes)) {
                ESP_LOGW(TAG, "Failed to parse storage_key UUID: %s", post->storage_key);
                continue;
            }
            memcpy(tmp.storage_key_uuid, uuid_bytes, sizeof(tmp.storage_key_uuid));
            tmp.extension = detect_file_type(post->art_url);
            tmp.artwork_modified_at = (uint32_t)parse_iso8601_utc(post->artwork_modified_at);
            tmp.dwell_time_ms = post->dwell_time_ms;
            tmp.total_artworks = 0;
        } else if (post->kind == MAKAPIX_POST_KIND_PLAYLIST) {
            tmp.extension = 0;
            tmp.artwork_modified_at = 0;
            tmp.dwell_time_ms = post->playlist_dwell_time_ms;
            tmp.total_artworks = post->total_artworks;
            memset(tmp.storage_key_uuid, 0, sizeof(tmp.storage_key_uuid));

            // Best-effort: write/update playlist cache on disk based on server response
            playlist_metadata_t playlist = {0};
            playlist.post_id = post->post_id;
            playlist.total_artworks = post->total_artworks;
            playlist.loaded_artworks = 0;
            playlist.available_artworks = 0;
            playlist.dwell_time_ms = post->playlist_dwell_time_ms;
            playlist.metadata_modified_at = parse_iso8601_utc(post->metadata_modified_at);

            if (post->artworks_count > 0 && post->artworks) {
                playlist.artworks = calloc(post->artworks_count, sizeof(artwork_ref_t));
                if (playlist.artworks) {
                    playlist.loaded_artworks = (int32_t)post->artworks_count;
                    for (size_t ai = 0; ai < post->artworks_count; ai++) {
                        const makapix_artwork_t *src = &post->artworks[ai];
                        artwork_ref_t *dst = &playlist.artworks[ai];
                        memset(dst, 0, sizeof(*dst));
                        dst->post_id = src->post_id;
                        strncpy(dst->storage_key, src->storage_key, sizeof(dst->storage_key) - 1);
                        strlcpy(dst->art_url, src->art_url, sizeof(dst->art_url));
                        dst->dwell_time_ms = src->dwell_time_ms;
                        dst->metadata_modified_at = parse_iso8601_utc(src->metadata_modified_at);
                        dst->artwork_modified_at = parse_iso8601_utc(src->artwork_modified_at);
                        dst->width = (uint16_t)src->width;
                        dst->height = (uint16_t)src->height;
                        dst->frame_count = (uint16_t)src->frame_count;
                        dst->has_transparency = src->has_transparency;

                        // Determine type from URL extension
                        switch (detect_file_type(src->art_url)) {
                            case EXT_WEBP: dst->type = ASSET_TYPE_WEBP; break;
                            case EXT_GIF:  dst->type = ASSET_TYPE_GIF;  break;
                            case EXT_PNG:  dst->type = ASSET_TYPE_PNG;  break;
                            case EXT_JPEG: dst->type = ASSET_TYPE_JPEG; break;
                            default:       dst->type = ASSET_TYPE_WEBP; break;
                        }

                        // Downloaded? (file exists in vault)
                        char vault_file[512];
                        build_vault_path_from_storage_key(ch, src->storage_key, detect_file_type(src->art_url), vault_file, sizeof(vault_file));
                        struct stat st;
                        dst->downloaded = (stat(vault_file, &st) == 0);
                        strlcpy(dst->filepath, vault_file, sizeof(dst->filepath));

                        // Opportunistic background download of the first PE items (single downloader).
                        // This is intentionally conservative: we only enqueue a limited prefix.
                        uint32_t pe_setting = config_store_get_pe();
                        uint32_t want = (pe_setting == 0) ? 32 : pe_setting;
                        if (want > 32) want = 32;
                        if (!dst->downloaded && ai < want) {
                            (void)download_queue_artwork(ch->channel_id, post->post_id, dst, DOWNLOAD_PRIORITY_LOW);
                        }
                    }

                    // available_artworks is informational only; keep it as "downloaded count"
                    int32_t cnt = 0;
                    for (int32_t j = 0; j < playlist.loaded_artworks; j++) {
                        if (playlist.artworks[j].downloaded) cnt++;
                    }
                    playlist.available_artworks = cnt;
                }
            }

            // Save and free temporary playlist structure
            if (playlist.artworks) {
                playlist_save_to_disk(&playlist);
                free(playlist.artworks);
                playlist.artworks = NULL;
            } else {
                // Still save minimal metadata (no artworks array)
                playlist_save_to_disk(&playlist);
            }
        } else {
            continue;
        }

        if (found_idx >= 0) {
            all_entries[(size_t)found_idx] = tmp;
        } else {
            all_entries[all_count++] = tmp;
        }
    }

    // Atomic write channel index (.bin)
    char temp_path[260];
    size_t path_len = strlen(index_path);
    if (path_len + 4 >= sizeof(temp_path)) {
        ESP_LOGE(TAG, "Index path too long for temp file");
        ret = ESP_ERR_INVALID_ARG;
        goto out;
    }
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", index_path);

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %d", temp_path, errno);
        ret = ESP_FAIL;
        goto out;
    }
    size_t written = fwrite(all_entries, sizeof(makapix_channel_entry_t), all_count, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (written != all_count) {
        ESP_LOGE(TAG, "Failed to write channel index: %zu/%zu", written, all_count);
        unlink(temp_path);
        ret = ESP_FAIL;
        goto out;
    }

    // FATFS rename() won't overwrite an existing destination: remove old index first.
    if (unlink(index_path) != 0) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "Failed to unlink old index before rename: %s (errno=%d)", index_path, errno);
        }
    }

    if (rename(temp_path, index_path) != 0) {
        const int rename_errno = errno;
#if MAKAPIX_TEMP_DEBUG_RENAME_FAIL
        // TEMP DEBUG: capture additional evidence around sporadic rename() failures (e.g. EEXIST=17 on FATFS)
        makapix_temp_debug_log_rename_failure(temp_path, index_path, rename_errno);
#endif
        // If unlink() above failed to remove the destination, try once more on EEXIST then retry rename.
        if (rename_errno == EEXIST) {
            (void)unlink(index_path);
            if (rename(temp_path, index_path) == 0) {
                goto rename_ok;
            }
        }

        // IMPORTANT: keep .tmp so boot-time recovery can promote it if needed.
        ESP_LOGE(TAG, "Failed to rename index temp file: %d", rename_errno);
        ret = ESP_FAIL;
        goto out;
    }

rename_ok:
    free(ch->entries);
    ch->entries = all_entries;
    ch->entry_count = all_count;

    ESP_LOGI(TAG, "Updated channel index: %zu total entries", all_count);
    ret = ESP_OK;
    all_entries = NULL; // ownership transferred to ch

out:
    if (ret != ESP_OK) {
        free(all_entries);
    }
    if (have_lock) {
        xSemaphoreGive(ch->index_io_lock);
    }
    return ret;
}

/**
 * @brief Queue background downloads for artwork posts that are not yet locally available
 * 
 * This function iterates through the provided posts and queues downloads for any
 * artwork posts whose files don't exist locally. Downloads happen in the background
 * via the download_manager, allowing playback to start immediately with available artworks.
 * 
 * Returns the number of items that could NOT be queued due to queue full (for backpressure).
 * 
 * @param ch Channel handle
 * @param posts Array of posts from server
 * @param count Number of posts
 * @return Number of artworks that couldn't be queued (0 = all queued successfully)
 */
static size_t queue_artwork_downloads(makapix_channel_t *ch, const makapix_post_t *posts, size_t count)
{
    if (!ch || !posts || count == 0) return 0;

    // Clear any previous cancellation for this channel so new downloads proceed
    download_manager_clear_cancelled(ch->channel_id);

    size_t queued = 0;
    size_t already_available = 0;
    size_t skipped_queue_full = 0;

    for (size_t i = 0; i < count; i++) {
        const makapix_post_t *post = &posts[i];

        if (post->kind != MAKAPIX_POST_KIND_ARTWORK) {
            // Playlist artworks are handled separately via their own download logic
            continue;
        }

        // Build the expected vault path for this artwork
        char vault_file[512];
        build_vault_path_from_storage_key(ch, post->storage_key, 
                                          detect_file_type(post->art_url), 
                                          vault_file, sizeof(vault_file));

        // Check if file already exists
        struct stat st;
        if (stat(vault_file, &st) == 0) {
            already_available++;
            continue;
        }

        // Check if queue has space before attempting to queue
        // Reserve some space for high-priority items
        size_t queue_space = download_manager_get_queue_space();
        if (queue_space < 4) {
            // Queue is nearly full - stop queueing and let refresh task handle backpressure
            skipped_queue_full++;
            continue;
        }

        // Queue download for this artwork
        download_request_t req = {0};
        req.playlist_post_id = 0;  // Not from a playlist
        req.artwork_post_id = post->post_id;
        strlcpy(req.storage_key, post->storage_key, sizeof(req.storage_key));
        strlcpy(req.art_url, post->art_url, sizeof(req.art_url));
        strlcpy(req.filepath, vault_file, sizeof(req.filepath));
        strlcpy(req.channel_id, ch->channel_id, sizeof(req.channel_id));
        
        // First few artworks get higher priority to enable quicker playback start
        if (queued < 3) {
            req.priority = DOWNLOAD_PRIORITY_HIGH;
        } else if (queued < 10) {
            req.priority = DOWNLOAD_PRIORITY_MEDIUM;
        } else {
            req.priority = DOWNLOAD_PRIORITY_LOW;
        }

        esp_err_t err = download_queue(&req);
        if (err == ESP_OK) {
            queued++;
        } else if (err == ESP_ERR_NO_MEM) {
            // Queue full - count this for backpressure
            skipped_queue_full++;
        } else {
            ESP_LOGW(TAG, "Failed to queue download for %s: %s", 
                     post->storage_key, esp_err_to_name(err));
        }
    }

    if (queued > 0 || already_available > 0) {
        ESP_LOGI(TAG, "Queued %zu downloads, %zu already available (channel=%s)", 
                 queued, already_available, ch->channel_id);
    }
    
    return skipped_queue_full;
}

// Helper: build vault path from storage_key (matches makapix_artwork_download pattern)
static void build_vault_path_from_storage_key(const makapix_channel_t *ch, const char *storage_key, 
                                               file_extension_t ext, char *out, size_t out_len)
{
    // Match makapix_artwork_download pattern: /vault/{dir1}/{dir2}/{storage_key}.{ext}
    uint32_t hash = hash_string(storage_key);
    char dir1[3], dir2[3];
    snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)((hash >> 24) & 0xFF));
    snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)((hash >> 16) & 0xFF));
    
    // Include file extension for type detection
    int ext_idx = (ext <= EXT_JPEG) ? ext : EXT_WEBP;
    snprintf(out, out_len, "%s/%s/%s/%s%s", 
             ch->vault_path, dir1, dir2, storage_key, s_ext_strings[ext_idx]);
}

/**
 * @brief Queue downloads starting from current play position in play-queue order
 * 
 * This ensures downloads happen in the order the user will encounter them,
 * respecting the current play order (server, random, created).
 * 
 * @param ch Channel handle
 * @param max_to_queue Maximum number of items to queue
 */
static void queue_downloads_by_play_position(makapix_channel_t *ch, size_t max_to_queue)
{
    if (!ch || !ch->navigator_ready || ch->entry_count == 0) return;
    
    download_manager_clear_cancelled(ch->channel_id);
    
    play_navigator_t *nav = &ch->navigator;
    if (!nav->order_indices || nav->order_count == 0) return;
    
    // Get current position
    uint32_t current_p = nav->p;
    size_t queued = 0;
    
    // Scan ahead from current position in play order
    for (size_t offset = 0; offset < nav->order_count && queued < max_to_queue; offset++) {
        // Position in play order (wrapping around)
        uint32_t pos_in_order = (current_p + (uint32_t)offset) % (uint32_t)nav->order_count;
        uint32_t entry_idx = nav->order_indices[pos_in_order];
        
        if (entry_idx >= ch->entry_count) continue;
        
        const makapix_channel_entry_t *entry = &ch->entries[entry_idx];
        
        // Only queue artwork posts (not playlists - those are handled separately)
        if (entry->kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) continue;
        
        // Build vault path
        char vault_path[512];
        build_vault_path(ch, entry, vault_path, sizeof(vault_path));
        
        // Check if already downloaded
        struct stat st;
        if (stat(vault_path, &st) == 0) continue;
        
        // Check queue space
        size_t queue_space = download_manager_get_queue_space();
        if (queue_space < 4) break;
        
        // Queue download
        download_request_t req = {0};
        req.playlist_post_id = 0;
        req.artwork_post_id = entry->post_id;
        bytes_to_uuid(entry->storage_key_uuid, req.storage_key, sizeof(req.storage_key));
        
        // Build art_url (we don't store it, but download_manager needs it)
        // NOTE: This is a limitation - we'd need to fetch from server or store in index
        // For now, skip items where we don't have the URL
        // Actually, makapix_artwork_download can work with just storage_key
        snprintf(req.art_url, sizeof(req.art_url), 
                 "https://%s/api/vault/%02x/%02x/%02x/%s%s",
                 CONFIG_MAKAPIX_CLUB_HOST,
                 entry->storage_key_uuid[0], entry->storage_key_uuid[1],
                 entry->storage_key_uuid[2], req.storage_key, 
                 s_ext_strings[entry->extension]);
        
        strlcpy(req.filepath, vault_path, sizeof(req.filepath));
        strlcpy(req.channel_id, ch->channel_id, sizeof(req.channel_id));
        
        // Priority based on distance from current position
        if (offset < 3) {
            req.priority = DOWNLOAD_PRIORITY_HIGH;
        } else if (offset < 10) {
            req.priority = DOWNLOAD_PRIORITY_MEDIUM;
        } else {
            req.priority = DOWNLOAD_PRIORITY_LOW;
        }
        
        if (download_queue(&req) == ESP_OK) {
            queued++;
        }
    }
    
    if (queued > 0) {
        ESP_LOGI(TAG, "Queued %zu downloads in play order (channel=%s, pos=%lu)", 
                 queued, ch->channel_id, (unsigned long)current_p);
    }
}

// Helper: evict excess artworks beyond limit
// NOTE: Per spec, limit applies to locally-downloaded files, not index entries
static esp_err_t evict_excess_artworks(makapix_channel_t *ch, size_t max_count)
{
    if (!ch) return ESP_ERR_INVALID_ARG;

    // Count artwork entries that actually have files downloaded (not just in index)
    // This is the key change: we only count files that exist locally
    size_t downloaded_count = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            char vault_path[512];
            build_vault_path(ch, &ch->entries[i], vault_path, sizeof(vault_path));
            struct stat st;
            if (stat(vault_path, &st) == 0) {
                downloaded_count++;
            }
        }
    }

    if (downloaded_count <= max_count) return ESP_OK;

    ESP_LOGI(TAG, "Eviction needed: %zu downloaded files exceed limit of %zu", 
             downloaded_count, max_count);

    // Collect only artwork entries that have files (for sorting by age)
    makapix_channel_entry_t *downloaded = malloc(downloaded_count * sizeof(makapix_channel_entry_t));
    if (!downloaded) return ESP_ERR_NO_MEM;

    size_t di = 0;
    for (size_t i = 0; i < ch->entry_count; i++) {
        if (ch->entries[i].kind == MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            char vault_path[512];
            build_vault_path(ch, &ch->entries[i], vault_path, sizeof(vault_path));
            struct stat st;
            if (stat(vault_path, &st) == 0) {
                downloaded[di++] = ch->entries[i];
            }
        }
    }

    // Sort by created_at (oldest first) - these are the candidates for deletion
    qsort(downloaded, downloaded_count, sizeof(makapix_channel_entry_t), compare_entries_by_created);

    // Per spec: "evicts the oldest 32 artworks" - evict in batches of 32 to make room
    const size_t EVICTION_BATCH = 32;
    size_t excess = downloaded_count - max_count;
    // Round up to nearest batch size
    size_t to_delete = ((excess + EVICTION_BATCH - 1) / EVICTION_BATCH) * EVICTION_BATCH;
    if (to_delete > downloaded_count) {
        to_delete = downloaded_count;
    }

    // Delete oldest artwork FILES (but keep their index entries - they can be re-downloaded)
    // The index entry remains so we know what should be there; the navigator will skip
    // artworks without files, and they'll be re-downloaded on next refresh if needed
    size_t actually_deleted = 0;
    for (size_t i = 0; i < to_delete; i++) {
        char vault_path[512];
        build_vault_path(ch, &downloaded[i], vault_path, sizeof(vault_path));
        if (unlink(vault_path) == 0) {
            actually_deleted++;
        }
    }

    free(downloaded);

    ESP_LOGI(TAG, "Evicted %zu artwork files to stay within limit of %zu", 
             actually_deleted, max_count);
    
    // NOTE: We intentionally do NOT remove entries from the index.
    // The index represents "what should be in this channel" and the server's view.
    // Files that were evicted can be re-downloaded when needed.
    // This matches the spec: "evicts (and deletes the artwork file)" - only the file, not the metadata.
    
    return ESP_OK;
}

// Background refresh task implementation
static void refresh_task_impl(void *pvParameters)
{
    makapix_channel_t *ch = (makapix_channel_t *)pvParameters;
    if (!ch) {
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Refresh task started for channel %s", ch->channel_id);
    
    // Determine channel type from channel_id
    makapix_channel_type_t channel_type = MAKAPIX_CHANNEL_ALL;
    makapix_query_request_t query_req = {0};
    
    if (strcmp(ch->channel_id, "all") == 0) {
        channel_type = MAKAPIX_CHANNEL_ALL;
    } else if (strcmp(ch->channel_id, "promoted") == 0) {
        channel_type = MAKAPIX_CHANNEL_PROMOTED;
    } else if (strcmp(ch->channel_id, "user") == 0) {
        channel_type = MAKAPIX_CHANNEL_USER;
    } else if (strncmp(ch->channel_id, "by_user_", 8) == 0) {
        channel_type = MAKAPIX_CHANNEL_BY_USER;
        strncpy(query_req.user_handle, ch->channel_id + 8, sizeof(query_req.user_handle) - 1);
    } else if (strncmp(ch->channel_id, "artwork_", 8) == 0) {
        // Single artwork channel - handled separately
        ch->refreshing = false;
        ch->refresh_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    query_req.channel = channel_type;
    query_req.sort = MAKAPIX_SORT_SERVER_ORDER; // Default
    query_req.limit = 32; // Query 32 at a time
    query_req.has_cursor = false;
    // Always send PE explicitly. p3a default is 8; user may set PE=0 ("all").
    query_req.pe_present = true;
    query_req.pe = (uint16_t)config_store_get_pe();
    
    const size_t TARGET_COUNT = 1024;  // Max locally-cached artworks per channel
    const uint32_t REFRESH_INTERVAL_SEC = 3600; // 1 hour
    
    while (ch->refreshing) {
        size_t total_queried = 0;
        query_req.has_cursor = false;
        query_req.cursor[0] = '\0';
        
        // Load saved cursor if exists (for continuing pagination)
        char saved_cursor[64] = {0};
        time_t last_refresh_time = 0;
        load_channel_metadata(ch, saved_cursor, &last_refresh_time);
        if (strlen(saved_cursor) > 0) {
            query_req.has_cursor = true;
            size_t copy_len = strlen(saved_cursor);
            if (copy_len >= sizeof(query_req.cursor)) copy_len = sizeof(query_req.cursor) - 1;
            memcpy(query_req.cursor, saved_cursor, copy_len);
            query_req.cursor[copy_len] = '\0';
        }
        
        // Allocate response on heap (too large for stack: ~23KB)
        makapix_query_response_t *resp = malloc(sizeof(makapix_query_response_t));
        if (!resp) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            break;
        }
        
        // Query posts until we have TARGET_COUNT or no more available
        while (total_queried < TARGET_COUNT && ch->refreshing) {
            memset(resp, 0, sizeof(makapix_query_response_t));
            esp_err_t err = makapix_api_query_posts(&query_req, resp);
            
            if (err != ESP_OK || !resp->success) {
                ESP_LOGW(TAG, "Query failed: %s", resp->error);
                break;
            }
            
            if (resp->post_count == 0) {
                ESP_LOGI(TAG, "No more posts available");
                break;
            }
            
            // Update channel index with new posts (metadata only, no downloads)
            update_index_bin(ch, resp->posts, resp->post_count);

            // Queue background downloads for artworks that aren't locally available
            // If navigator is ready, use play-position-based queueing for proper order
            // Otherwise, fall back to server-order queueing
            size_t skipped = 0;
            if (ch->navigator_ready) {
                // Queue downloads in play-queue order, starting from current position
                queue_downloads_by_play_position(ch, 32);
            } else {
                // Navigator not ready - use server order (happens during initial load)
                skipped = queue_artwork_downloads(ch, resp->posts, resp->post_count);
            }

            // If Live Mode is active, mark schedule dirty so it is rebuilt on next access.
            if (ch->navigator_ready && ch->navigator.live_mode) {
                play_navigator_mark_live_dirty(&ch->navigator);
            }

            // Free any heap allocations inside parsed posts (playlist artworks arrays)
            for (size_t pi = 0; pi < resp->post_count; pi++) {
                if (resp->posts[pi].kind == MAKAPIX_POST_KIND_PLAYLIST && resp->posts[pi].artworks) {
                    free(resp->posts[pi].artworks);
                    resp->posts[pi].artworks = NULL;
                    resp->posts[pi].artworks_count = 0;
                }
            }
            total_queried += resp->post_count;
            
            // Save cursor for next query
            if (resp->has_more && strlen(resp->next_cursor) > 0) {
                query_req.has_cursor = true;
                size_t copy_len = strlen(resp->next_cursor);
                if (copy_len >= sizeof(query_req.cursor)) copy_len = sizeof(query_req.cursor) - 1;
                memcpy(query_req.cursor, resp->next_cursor, copy_len);
                query_req.cursor[copy_len] = '\0';
            } else {
                query_req.has_cursor = false;
            }
            
            // Note: update_index_bin() already updated ch->entries and ch->entry_count
            // No need to reload from disk - that would overwrite our in-memory state
            
            if (!resp->has_more) break;
            
            // BACKPRESSURE: Wait for download queue to drain before querying next batch
            // This prevents overwhelming the queue and the SDIO bus
            if (skipped > 0) {
                ESP_LOGI(TAG, "Download queue full (%zu skipped), waiting for space...", skipped);
            }
            
            // Wait until queue has space for at least half a batch (16 items)
            // Use a reasonable timeout to avoid indefinite blocking
            const size_t MIN_SPACE_FOR_NEXT_BATCH = 16;
            const uint32_t MAX_WAIT_MS = 60000; // 60 seconds max wait
            
            if (!download_manager_wait_for_space(MIN_SPACE_FOR_NEXT_BATCH, MAX_WAIT_MS)) {
                // If we can't queue downloads, don't keep querying pages: it grows the index and
                // makes the system look "stuck" (no files become available).
                // Instead, back off and try again later while the downloader drains.
                ESP_LOGW(TAG, "Timed out waiting for download queue space, backing off before next query");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            
            // Additional delay to let SD card writes complete and reduce bus contention
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        
        free(resp);
        
        // Evict excess artworks
        evict_excess_artworks(ch, TARGET_COUNT);
        
        // Save metadata
        time_t now = time(NULL);
        save_channel_metadata(ch, query_req.has_cursor ? query_req.cursor : "", now);
        ch->last_refresh_time = now;
        
        // Note: Channel state is already up-to-date from update_index_bin()
        // No need to reload - the in-memory state has all entries
        
        ESP_LOGI(TAG, "Refresh cycle completed: queried %zu posts, channel has %zu entries", 
                 total_queried, ch->entry_count);
        
        // Wait 1 hour before next refresh (or until cancelled)
        for (uint32_t elapsed = 0; elapsed < REFRESH_INTERVAL_SEC && ch->refreshing; elapsed++) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
        }
        
        if (!ch->refreshing) {
            break; // Task was cancelled
        }
    }
    
    ESP_LOGI(TAG, "Refresh task exiting");
    ch->refreshing = false;
    ch->refresh_task = NULL;
    vTaskDelete(NULL);
}

static void makapix_impl_destroy(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return;
    
    // Stop refresh task if running
    if (ch->refreshing && ch->refresh_task) {
        ch->refreshing = false;
        // Give task a moment to exit gracefully
        vTaskDelay(pdMS_TO_TICKS(100));
        if (ch->refresh_task) {
            vTaskDelete(ch->refresh_task);
        }
        ch->refresh_task = NULL;
    }
    
    makapix_impl_unload(channel);
    if (ch->index_io_lock) {
        vSemaphoreDelete(ch->index_io_lock);
        ch->index_io_lock = NULL;
    }
    free(ch->channel_id);
    free(ch->vault_path);
    free(ch->channels_path);
    free(ch->base.name);
    free(ch);
    
    ESP_LOGI(TAG, "Channel destroyed");
}

static void *makapix_impl_get_navigator(channel_handle_t channel)
{
    makapix_channel_t *ch = (makapix_channel_t *)channel;
    if (!ch) return NULL;
    return ch->navigator_ready ? (void *)&ch->navigator : NULL;
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

    ch->index_io_lock = xSemaphoreCreateMutex();
    if (!ch->index_io_lock) {
        free(ch->base.name);
        free(ch->channel_id);
        free(ch->vault_path);
        free(ch->channels_path);
        free(ch);
        return NULL;
    }
    
    if (!ch->base.name || !ch->channel_id || !ch->vault_path || !ch->channels_path) {
        vSemaphoreDelete(ch->index_io_lock);
        ch->index_io_lock = NULL;
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

esp_err_t makapix_channel_count_cached(const char *channel_id,
                                        const char *channels_path,
                                        const char *vault_path,
                                        size_t *out_total,
                                        size_t *out_cached)
{
    if (!channel_id || !channels_path || !vault_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build index path
    char index_path[256];
    snprintf(index_path, sizeof(index_path), "%s/%s.bin", channels_path, channel_id);
    
    // Try to open index file
    FILE *f = fopen(index_path, "rb");
    if (!f) {
        // No index file - channel has never been loaded
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size % sizeof(makapix_channel_entry_t) != 0) {
        fclose(f);
        if (out_total) *out_total = 0;
        if (out_cached) *out_cached = 0;
        return ESP_OK;
    }
    
    size_t entry_count = (size_t)(file_size / sizeof(makapix_channel_entry_t));
    if (out_total) *out_total = entry_count;
    
    // Count cached artworks (ones that exist locally)
    size_t cached_count = 0;
    makapix_channel_entry_t entry;
    
    for (size_t i = 0; i < entry_count; i++) {
        if (fread(&entry, sizeof(entry), 1, f) != 1) {
            break;
        }
        
        // Only count artwork posts (not playlists)
        if (entry.kind != MAKAPIX_INDEX_POST_KIND_ARTWORK) {
            continue;
        }
        
        // Build vault path from storage_key_uuid
        char storage_key[37];
        bytes_to_uuid(entry.storage_key_uuid, storage_key, sizeof(storage_key));
        
        uint32_t hash = hash_string(storage_key);
        char dir1[3], dir2[3];
        snprintf(dir1, sizeof(dir1), "%02x", (unsigned int)((hash >> 24) & 0xFF));
        snprintf(dir2, sizeof(dir2), "%02x", (unsigned int)((hash >> 16) & 0xFF));
        
        int ext_idx = (entry.extension <= EXT_JPEG) ? entry.extension : EXT_WEBP;
        char file_path[256];
        snprintf(file_path, sizeof(file_path), "%s/%s/%s/%s%s",
                 vault_path, dir1, dir2, storage_key, s_ext_strings[ext_idx]);
        
        struct stat st;
        if (stat(file_path, &st) == 0) {
            cached_count++;
        }
    }
    
    fclose(f);
    
    if (out_cached) *out_cached = cached_count;
    return ESP_OK;
}

