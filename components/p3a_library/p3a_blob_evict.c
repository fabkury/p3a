// SPDX-License-Identifier: Apache-2.0
// Making room on the card: the walk over the blob store. What goes is
// decided in p3a_evict.c.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_blob.h"
#include "p3a_clock.h"
#include "p3a_pins.h"
#include "p3a_err.h"
#include "p3a_evict.h"
#include "p3a_storage.h"
#include "psram_alloc.h"
#include "sd_health.h"
#include "sd_path.h"

static const char *TAG = "evict";

typedef struct {
    p3a_evict_entry_t *entries;
    uint32_t count;
    uint32_t skipped;  // beyond what one run looks at
    p3a_blob_keep_fn keep;
    void *ctx;
} walk_t;

// The store is {blobs}/{d0}/{d1}/{file}: two levels of folders, then files.
static void walk(char *path, size_t cap, int depth, walk_t *w)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return;
    }
    const size_t base = strlen(path);
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.' || base + 1 + strlen(e->d_name) >= cap) {
            continue;
        }
        snprintf(path + base, cap - base, "/%s", e->d_name);
        if (depth > 0) {
            walk(path, cap, depth - 1, w);
            continue;
        }
        p3a_evict_entry_t entry = {0};
        p3a_format_t format;
        struct stat st;
        if (!p3a_evict_parse_name(e->d_name, entry.ref, &format) || stat(path, &st) != 0) {
            continue;  // not a blob: not ours to judge
        }
        if (w->count == P3A_EVICT_MAX_ENTRIES) {
            w->skipped++;
            continue;
        }
        entry.format = (uint8_t)format;
        entry.modified_at = st.st_mtime > 0 ? (uint32_t)st.st_mtime : 0;
        entry.size = (uint32_t)st.st_size;
        // A pinned artwork is never evicted, whoever asks and whatever they pass.
        entry.keep = p3a_pins_keeps(entry.ref) || (w->keep != NULL && w->keep(entry.ref, w->ctx));
        w->entries[w->count++] = entry;
        if (w->count % 64 == 0) {
            vTaskDelay(1);  // thousands of files: the others must run
        }
    }
    path[base] = '\0';
    closedir(dir);
}

esp_err_t p3a_blob_evict(uint64_t need_bytes, uint32_t min_age_s, p3a_blob_keep_fn keep, void *ctx,
                         p3a_blob_evict_t *out)
{
    p3a_blob_evict_t result = {0};
    if (out != NULL) {
        *out = result;
    }
    if (sd_health_is_failed() || p3a_storage_is_handed_over()) {
        return P3A_ERR_SD_FAILED;  // never delete on a card that may be damaged, or that a computer has
    }
    if (!p3a_clock_is_set()) {
        return ESP_ERR_INVALID_STATE;
    }
    walk_t w = {.entries = psram_malloc(P3A_EVICT_MAX_ENTRIES * sizeof(p3a_evict_entry_t)), .keep = keep, .ctx = ctx};
    if (w.entries == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char path[P3A_BLOB_PATH_MAX];
    esp_err_t err = sd_path_get_subdir("blobs", path, sizeof(path));
    if (err == ESP_OK) {
        walk(path, sizeof(path), SD_SHARD_DEPTH, &w);
        result.seen = w.count + w.skipped;
        const uint32_t n = p3a_evict_choose(w.entries, w.count, need_bytes, (uint32_t)time(NULL), min_age_s, NULL);
        for (uint32_t i = 0; i < n; i++) {
            const p3a_evict_entry_t *e = &w.entries[i];
            if (p3a_pins_keeps(e->ref)) {
                continue;  // pinned since the walk
            }
            if (p3a_blob_remove(e->ref, (p3a_format_t)e->format) == ESP_OK) {
                result.deleted++;
                result.freed_bytes += e->size;
            }
            if (i % 16 == 15) {
                vTaskDelay(1);
            }
        }
        ESP_LOGI(TAG, "%u blobs in the store; %u deleted, %u KB freed of %u KB wanted", (unsigned)result.seen,
                 (unsigned)result.deleted, (unsigned)(result.freed_bytes >> 10), (unsigned)(need_bytes >> 10));
    }
    free(w.entries);
    if (out != NULL) {
        *out = result;
    }
    return err;
}
