// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file klipy_refresh.c
 * @brief Klipy channel refresh - fetches trending/search/category and merges
 *
 * Mirrors giphy_refresh.c (merge / eviction / partial-refresh contract are
 * identical). Differences: Klipy pagination is page-based with a has_next flag
 * (not offset-based), and the channel sub-type comes from spec_name
 * "{product}:{mode}" (product in {gif,sticker}; mode in {trending,search,
 * category} — category is served by the search endpoint using its query).
 */

#include "klipy.h"
#include "klipy_types.h"
#include "config_store.h"
#include "p3a_board.h"
#include "channel_cache.h"
#include "channel_metadata.h"
#include "download_manager.h"
#include "sntp_sync.h"
#include "makapix_channel_events.h"
#include "sd_path.h"
#include "play_scheduler.h"  // ps_get_display_name()
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "psram_alloc.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static const char *TAG = "klipy_refresh";

// Si hash node — tracks post_ids seen during a refresh cycle
typedef struct {
    int32_t post_id;
    UT_hash_handle hh;
} si_node_t;

static volatile bool s_refresh_cancel = false;
static klipy_refresh_status_t s_last_refresh_status = KLIPY_REFRESH_NOT_ATTEMPTED;

void klipy_cancel_refresh(void)
{
    s_refresh_cancel = true;
    ESP_LOGI(TAG, "Klipy refresh cancellation requested");
}

bool klipy_is_refresh_cancelled(void)
{
    return s_refresh_cancel;
}

// ---- Merge (dedup by post_id; entries reinterpret as makapix_channel_entry_t) ----
static esp_err_t klipy_merge_entries(channel_cache_t *cache,
                                     const klipy_channel_entry_t *new_entries,
                                     size_t new_count,
                                     size_t max_entries)
{
    if (!cache || !new_entries || new_count == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    size_t max_total = cache->entry_count + new_count;
    if (max_total > max_entries) max_total = max_entries;

    makapix_channel_entry_t *all = psram_malloc(max_total * sizeof(makapix_channel_entry_t));
    if (!all) {
        xSemaphoreGive(cache->mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t all_count = 0;
    if (cache->entries && cache->entry_count > 0) {
        size_t copy_count = cache->entry_count;
        if (copy_count > max_total) copy_count = max_total;
        memcpy(all, cache->entries, copy_count * sizeof(makapix_channel_entry_t));
        all_count = copy_count;
    }

    for (size_t i = 0; i < new_count && all_count < max_total; i++) {
        const klipy_channel_entry_t *ne = &new_entries[i];
        bool found = false;
        for (size_t j = 0; j < all_count; j++) {
            if (all[j].post_id == ne->post_id) {
                memcpy(&all[j], ne, sizeof(makapix_channel_entry_t));
                found = true;
                break;
            }
        }
        if (!found) {
            memcpy(&all[all_count], ne, sizeof(makapix_channel_entry_t));
            all_count++;
        }
    }

    if (cache->entries) {
        free(cache->entries);
    }
    cache->entries = all;
    cache->entry_count = all_count;

    ci_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->post_id_hash, node, tmp) {
        HASH_DEL(cache->post_id_hash, node);
        free(node);
    }
    cache->post_id_hash = NULL;

    for (size_t i = 0; i < all_count; i++) {
        ci_post_id_node_t *n = psram_malloc(sizeof(ci_post_id_node_t));
        if (n) {
            n->post_id = all[i].post_id;
            n->ci_index = (uint32_t)i;
            HASH_ADD_INT(cache->post_id_hash, post_id, n);
        }
    }

    cache->dirty = true;
    xSemaphoreGive(cache->mutex);
    channel_cache_schedule_save(cache);
    return ESP_OK;
}

// ---- Evict entries not seen this refresh cycle (delete their cached files) ----
static void klipy_evict_orphans(channel_cache_t *cache, si_node_t *si_hash)
{
    if (!cache) return;

    int32_t *evicted_ids = NULL;
    size_t evicted = 0;

    xSemaphoreTake(cache->mutex, portMAX_DELAY);

    if (cache->entry_count > 0) {
        evicted_ids = psram_malloc(cache->entry_count * sizeof(int32_t));
    }

    size_t kept = 0;
    for (size_t i = 0; i < cache->entry_count; i++) {
        si_node_t *found = NULL;
        HASH_FIND_INT(si_hash, &cache->entries[i].post_id, found);
        if (found) {
            if (kept != i) {
                memcpy(&cache->entries[kept], &cache->entries[i],
                       sizeof(makapix_channel_entry_t));
            }
            kept++;
        } else {
            const klipy_channel_entry_t *ke =
                (const klipy_channel_entry_t *)&cache->entries[i];
            char filepath[256];
            klipy_build_filepath(ke->klipy_id, ke->product, ke->extension,
                                 filepath, sizeof(filepath));
            unlink(filepath);
            strlcat(filepath, ".404", sizeof(filepath));
            unlink(filepath);
            if (evicted_ids) {
                evicted_ids[evicted] = ke->post_id;
            }
            evicted++;
        }
    }

    cache->entry_count = kept;

    ci_post_id_node_t *node, *tmp;
    HASH_ITER(hh, cache->post_id_hash, node, tmp) {
        HASH_DEL(cache->post_id_hash, node);
        free(node);
    }
    cache->post_id_hash = NULL;

    for (size_t i = 0; i < kept; i++) {
        ci_post_id_node_t *n = psram_malloc(sizeof(ci_post_id_node_t));
        if (n) {
            n->post_id = cache->entries[i].post_id;
            n->ci_index = (uint32_t)i;
            HASH_ADD_INT(cache->post_id_hash, post_id, n);
        }
    }

    cache->dirty = true;
    xSemaphoreGive(cache->mutex);

    for (size_t i = 0; i < evicted; i++) {
        int removed_pos = -1;
        if (lai_remove_entry(cache, evicted_ids[i], &removed_pos)) {
            play_scheduler_compensate_cursor_after_lai_remove(cache, removed_pos);
        }
    }
    free(evicted_ids);

    channel_cache_schedule_save(cache);
    ESP_LOGI(TAG, "Full refresh: evicted %zu orphaned entries, %zu kept", evicted, kept);
}

// Klipy payloads are larger than Giphy (per-tier renditions + base64
// blur_preview, and no fields= filter param), so allow more headroom.
#define KLIPY_RESPONSE_BUF_SIZE (512 * 1024)

// Wrap the per-playset offset; Klipy trending reaches at least ~2500 items.
#define KLIPY_OFFSET_CAP 2500

esp_err_t klipy_refresh_channel(const char *channel_id, const char *spec_name,
                                const char *identifier, uint32_t channel_offset)
{
    return klipy_refresh_channel_with_progress(channel_id, spec_name, identifier,
                                               channel_offset, NULL, NULL);
}

esp_err_t klipy_refresh_channel_with_progress(const char *channel_id, const char *spec_name,
                                              const char *identifier, uint32_t channel_offset,
                                              klipy_refresh_progress_cb_t progress_cb,
                                              void *progress_ctx)
{
    if (!channel_id || !spec_name) return ESP_ERR_INVALID_ARG;

    char _dn[64];
    ps_get_display_name(channel_id, _dn, sizeof(_dn));

    if (klipy_is_rate_limited()) {
        ESP_LOGW(TAG, "Skipping '%s': Klipy rate-limited (%us remaining)",
                 _dn, (unsigned)klipy_cooldown_remaining_sec());
        s_last_refresh_status = KLIPY_REFRESH_FAILED;
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Parse spec_name "{product}:{mode}".
    char product_str[16] = "gif";
    char mode[16] = "trending";
    const char *colon = strchr(spec_name, ':');
    if (colon) {
        size_t plen = (size_t)(colon - spec_name);
        if (plen >= sizeof(product_str)) plen = sizeof(product_str) - 1;
        memcpy(product_str, spec_name, plen);
        product_str[plen] = '\0';
        strlcpy(mode, colon + 1, sizeof(mode));
    } else {
        strlcpy(mode, spec_name, sizeof(mode));
    }
    uint8_t product_id = (strcmp(product_str, "sticker") == 0)
                             ? KLIPY_PRODUCT_STICKER : KLIPY_PRODUCT_GIF;
    // category is served by the search endpoint using its query.
    bool query_mode = (strcmp(mode, "search") == 0 || strcmp(mode, "category") == 0);

    ESP_LOGI(TAG, "Refreshing Klipy channel: %s (%s/%s)", _dn, product_str, mode);

    s_refresh_cancel = false;

    channel_cache_lifecycle_lock();
    bool cache_exists = (channel_cache_registry_find(channel_id) != NULL);
    channel_cache_lifecycle_unlock();
    if (!cache_exists) {
        ESP_LOGW(TAG, "Channel cache not found for '%s'", _dn);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t cache_size = config_store_get_klipy_cache_size();

    klipy_fetch_ctx_t ctx = {0};
    config_store_get_klipy_api_key(ctx.api_key, sizeof(ctx.api_key));
    if (ctx.api_key[0] == '\0') {
        ESP_LOGE(TAG, "No Klipy API key configured");
        klipy_set_no_key();
        s_last_refresh_status = KLIPY_REFRESH_NO_API_KEY;
        return ESP_ERR_NOT_FOUND;
    }

    if (klipy_auth_invalid_for_key(ctx.api_key) &&
        !config_store_get_refresh_allow_override()) {
        ESP_LOGW(TAG, "Skipping '%s': Klipy API key marked invalid (reprobe in %us)",
                 _dn, (unsigned)klipy_auth_invalid_remaining_sec());
        s_last_refresh_status = KLIPY_REFRESH_INVALID_API_KEY;
        return ESP_ERR_NOT_ALLOWED;
    }

    strlcpy(ctx.product, product_id == KLIPY_PRODUCT_STICKER ? "stickers" : "gifs",
            sizeof(ctx.product));
    ctx.product_id = product_id;
    if (config_store_get_klipy_format(ctx.format, sizeof(ctx.format)) != ESP_OK) {
        strlcpy(ctx.format, CONFIG_KLIPY_FORMAT_DEFAULT, sizeof(ctx.format));
    }
    if (config_store_get_klipy_rating(ctx.rating, sizeof(ctx.rating)) != ESP_OK) {
        strlcpy(ctx.rating, CONFIG_KLIPY_RATING_DEFAULT, sizeof(ctx.rating));
    }
    ctx.screen_width = P3A_DISPLAY_WIDTH;
    ctx.screen_height = P3A_DISPLAY_HEIGHT;

    if (query_mode && identifier && identifier[0] != '\0') {
        strlcpy(ctx.query, identifier, sizeof(ctx.query));
        ESP_LOGI(TAG, "Query mode: q=\"%s\"", ctx.query);
    } else {
        ctx.query[0] = '\0';  // trending
    }

    ctx.response_buf_size = KLIPY_RESPONSE_BUF_SIZE;
    ctx.response_buf = heap_caps_malloc(ctx.response_buf_size, MALLOC_CAP_SPIRAM);
    if (!ctx.response_buf) {
        ctx.response_buf = malloc(ctx.response_buf_size);
        if (!ctx.response_buf) {
            ESP_LOGE(TAG, "Failed to allocate response buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    extern void download_manager_rescan(void);

    klipy_channel_entry_t *page_entries = heap_caps_malloc(
        KLIPY_PAGE_LIMIT * sizeof(klipy_channel_entry_t), MALLOC_CAP_SPIRAM);
    if (!page_entries) {
        page_entries = malloc(KLIPY_PAGE_LIMIT * sizeof(klipy_channel_entry_t));
        if (!page_entries) {
            ESP_LOGE(TAG, "Failed to allocate page_entries buffer");
            free(ctx.response_buf);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total_fetched = 0;
    // Page-based pagination (1-based). The per-playset offset maps to a start
    // page; pagination also stops naturally on has_next == false.
    int start_offset = (int)(channel_offset % (uint32_t)KLIPY_OFFSET_CAP);
    int page = start_offset / KLIPY_PAGE_LIMIT + 1;
    esp_err_t last_err = ESP_OK;
    bool refresh_completed = true;

    si_node_t *si_hash = NULL;
    size_t si_count = 0;

    while (total_fetched < cache_size) {
        if (s_refresh_cancel) {
            ESP_LOGI(TAG, "Refresh cancelled before page fetch (page=%d)", page);
            refresh_completed = false;
            break;
        }

        size_t page_count = 0;
        bool has_more = false;
        esp_err_t err = klipy_fetch_page(&ctx, page, page_entries, &page_count, &has_more);

        if (s_refresh_cancel) {
            ESP_LOGI(TAG, "Refresh cancelled after page fetch (page=%d)", page);
            refresh_completed = false;
            break;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Page fetch failed at page=%d: %s", page, esp_err_to_name(err));
            last_err = err;
            refresh_completed = false;
            break;
        }

        if (page_count == 0) {
            ESP_LOGI(TAG, "No entries returned at page=%d, done", page);
            break;
        }

        size_t merge_limit = cache_size * 3;
        channel_cache_lifecycle_lock();
        channel_cache_t *page_cache = channel_cache_registry_find(channel_id);
        esp_err_t merge_err = page_cache
            ? klipy_merge_entries(page_cache, page_entries, page_count, merge_limit)
            : ESP_ERR_NOT_FOUND;
        channel_cache_lifecycle_unlock();
        if (merge_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Cache disappeared mid-refresh for '%s', aborting", _dn);
            refresh_completed = false;
            break;
        }
        if (merge_err != ESP_OK) {
            ESP_LOGW(TAG, "Merge failed at page=%d: %s", page, esp_err_to_name(merge_err));
            refresh_completed = false;
            break;
        }

        for (size_t i = 0; i < page_count && si_count < cache_size; i++) {
            int32_t pid = page_entries[i].post_id;
            si_node_t *existing = NULL;
            HASH_FIND_INT(si_hash, &pid, existing);
            if (!existing) {
                si_node_t *n = psram_malloc(sizeof(si_node_t));
                if (n) {
                    n->post_id = pid;
                    HASH_ADD_INT(si_hash, post_id, n);
                    si_count++;
                }
            }
        }

        total_fetched += page_count;
        ESP_LOGI(TAG, "Page merged: %zu entries (total: %zu)", page_count, total_fetched);

        download_manager_rescan();

        if (progress_cb) {
            progress_cb((int)total_fetched, (int)cache_size, progress_ctx);
        }

        page++;

        if (!has_more) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(ctx.response_buf);
    free(page_entries);

    if (refresh_completed && si_hash) {
        channel_cache_lifecycle_lock();
        channel_cache_t *evict_cache = channel_cache_registry_find(channel_id);
        if (evict_cache) {
            klipy_evict_orphans(evict_cache, si_hash);
        }
        channel_cache_lifecycle_unlock();
    }

    {
        si_node_t *node, *tmp;
        HASH_ITER(hh, si_hash, node, tmp) {
            HASH_DEL(si_hash, node);
            free(node);
        }
        si_hash = NULL;
    }

    if (refresh_completed && sntp_sync_is_synchronized()) {
        char channels_path[128];
        esp_err_t path_err = sd_path_get_channel(channels_path, sizeof(channels_path));
        if (path_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot resolve channel directory (%s) - skipping metadata save",
                     esp_err_to_name(path_err));
        } else {
            channel_metadata_t meta = {
                .last_refresh = time(NULL),
                .cursor = "",
            };
            esp_err_t meta_err = channel_metadata_save(channel_id, channels_path, &meta);
            if (meta_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save channel metadata: %s", esp_err_to_name(meta_err));
            }
        }
    } else if (refresh_completed) {
        ESP_LOGI(TAG, "Clock not synchronized, deferring metadata save for '%s'", channel_id);
    }

    size_t final_entry_count = 0;
    {
        channel_cache_lifecycle_lock();
        channel_cache_t *final_cache = channel_cache_registry_find(channel_id);
        if (final_cache) final_entry_count = final_cache->entry_count;
        channel_cache_lifecycle_unlock();
    }
    ps_get_display_name(channel_id, _dn, sizeof(_dn));
    ESP_LOGI(TAG, "Klipy channel '%s' refresh %s: %zu fetched, %zu in cache",
             _dn, refresh_completed ? "complete" : "incomplete",
             total_fetched, final_entry_count);

    if (total_fetched > 0) {
        klipy_clear_auth_invalid();
        s_last_refresh_status = KLIPY_REFRESH_OK;
    } else if (last_err == ESP_ERR_NOT_ALLOWED) {
        s_last_refresh_status = KLIPY_REFRESH_INVALID_API_KEY;
    } else {
        s_last_refresh_status = KLIPY_REFRESH_FAILED;
    }

    if (!refresh_completed) {
        return (last_err != ESP_OK) ? last_err : ESP_FAIL;
    }
    if (total_fetched > 0) {
        return ESP_OK;
    }
    return (last_err != ESP_OK) ? last_err : ESP_FAIL;
}

klipy_refresh_status_t klipy_get_last_refresh_status(void)
{
    return s_last_refresh_status;
}
