// SPDX-License-Identifier: Apache-2.0
#include "source_local.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_err.h"
#include "p3a_source.h"
#include "psram_alloc.h"
#include "sd_path.h"

static const char *TAG = "local";

static source_local_scan_t s_last;
static SemaphoreHandle_t s_lock;  // guards s_last

// Newest first; files of the same second in name order, so that the order
// does not depend on how the card happens to list its folder.
static int by_recency(const void *a, const void *b)
{
    const p3a_record_t *ra = a, *rb = b;
    if (ra->sort_at != rb->sort_at) {
        return ra->sort_at > rb->sort_at ? -1 : 1;
    }
    return strcmp(ra->identity.local.filename, rb->identity.local.filename);
}

static esp_err_t local_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    (void)channel;  // the local source has one channel: the folder
    const int64_t t0 = esp_timer_get_time();
    source_local_scan_t scan = {0};
    char dir[160];
    esp_err_t err = sd_path_get_animations(dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "cannot read %s (errno %d)", dir, errno);
            return ESP_FAIL;
        }
        p3a_index_clear(index);  // no folder: no files
        return ESP_OK;
    }
    p3a_record_t *records = psram_malloc((size_t)index->capacity * sizeof(p3a_record_t));
    if (records == NULL) {
        closedir(d);
        return ESP_ERR_NO_MEM;
    }
    uint32_t count = 0, seen = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if ((++seen & 15) == 0) {
            vTaskDelay(1);  // thousands of files: let the others run
        }
        const p3a_format_t format = source_local_format_of(entry->d_name);
        if (format == P3A_FORMAT_UNKNOWN) {
            scan.not_pictures++;
            continue;
        }
        char path[sizeof(dir) + 260];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            scan.not_pictures++;  // a folder named like a picture, or gone since it was listed
            continue;
        }
        p3a_record_t made;
        if (p3a_record_init_local(&made, entry->d_name) != ESP_OK) {
            scan.name_too_long++;
            continue;
        }
        if (count >= index->capacity) {
            scan.over_capacity++;
            continue;
        }
        p3a_record_t *r = &records[count];
        *r = made;
        r->format = (uint8_t)format;
        r->flags = P3A_RECORD_IN_PLACE;
        r->sort_at = (uint32_t)st.st_mtime;
        r->modified_at = (uint32_t)st.st_mtime;
        count++;
    }
    closedir(d);
    qsort(records, count, sizeof(p3a_record_t), by_recency);
    err = p3a_index_set_records(index, records, count);
    free(records);

    scan.listed = err == ESP_OK ? count : 0;
    scan.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (scan.name_too_long || scan.over_capacity) {
        ESP_LOGW(TAG, "%u files left out: %u names too long, %u beyond the %u the index holds",
                 (unsigned)(scan.name_too_long + scan.over_capacity), (unsigned)scan.name_too_long,
                 (unsigned)scan.over_capacity, (unsigned)index->capacity);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last = scan;
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t local_path(const p3a_record_t *record, char *out, size_t len)
{
    char dir[160];
    esp_err_t err = sd_path_get_animations(dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    const int n = snprintf(out, len, "%s/%s", dir, record->identity.local.filename);
    return (n > 0 && (size_t)n < len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static uint32_t local_index_capacity(void)
{
    return P3A_INDEX_MAX_RECORDS;
}

static uint32_t local_refresh_interval_s(void)
{
    return 0;  // listed again only when something says the folder changed
}

void source_local_last_scan(source_local_scan_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_lock);
}

esp_err_t source_local_start(void)
{
    static const p3a_source_ops_t ops = {
        .source = P3A_SOURCE_LOCAL,
        .name = "local",
        .refresh = local_refresh,
        .path = local_path,
        .refresh_interval_s = local_refresh_interval_s,
        .index_capacity = local_index_capacity,
        .smooth = false,  // the user's files are shown as they are, pixel for pixel
    };
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return p3a_source_register(&ops);
}
