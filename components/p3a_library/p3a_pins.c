// SPDX-License-Identifier: Apache-2.0
// Pin lists on the card. See p3a_pins.h. The table of lists and the set of
// kept blobs are pure code in p3a_pin_table.c; this file is the files and
// the lock.

#include "p3a_pins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_frame.h"
#include "p3a_storage.h"
#include "psram_alloc.h"
#include "sd_path.h"

static const char *TAG = "pins";

#define FIRST_LIST_NAME "My Pins"

static SemaphoreHandle_t s_lock;
static p3a_pin_table_t *s_table;  // in PSRAM: 5 KB
static p3a_pin_keep_t s_keep;
// How many pins each list holds, beside the table (same positions): the
// lists are asked for often, and counting should not read 64 files.
static uint32_t s_counts[P3A_PINS_MAX_LISTS];
static uint32_t s_revision;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static esp_err_t table_path(char *out, size_t len)
{
    char dir[128];
    const esp_err_t err = sd_path_get_subdir("pins", dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    return snprintf(out, len, "%s/lists.tbl", dir) < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t list_path(const char *slug, char *out, size_t len)
{
    char dir[128];
    const esp_err_t err = sd_path_get_subdir("pins", dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    return snprintf(out, len, "%s/%s.idx", dir, slug) < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t table_save(void)
{
    char path[P3A_FS_PATH_MAX];
    esp_err_t err = table_path(path, sizeof(path));
    if (err == ESP_OK) {
        err = p3a_frame_save(path, P3A_FRAME_SCHEMA_PIN_TABLE, P3A_PIN_TABLE_VERSION, sizeof(p3a_pin_list_t),
                             s_table->count, P3A_FRAME_USER_DATA, s_table->lists);
    }
    return err;
}

// A list's records into a fresh index. A list with no file yet is empty.
static esp_err_t list_load(const char *slug, p3a_index_t *index)
{
    char path[P3A_FS_PATH_MAX];
    esp_err_t err = list_path(slug, path, sizeof(path));
    if (err == ESP_OK) {
        err = p3a_index_load(index, path);
    }
    if (err == (esp_err_t)P3A_ERR_MISSING) {
        p3a_index_clear(index);
        err = ESP_OK;
    }
    return err;
}

static esp_err_t list_save(const char *slug, const p3a_index_t *index)
{
    char path[P3A_FS_PATH_MAX];
    esp_err_t err = list_path(slug, path, sizeof(path));
    if (err == ESP_OK) {
        err = p3a_index_save(index, path, P3A_FRAME_USER_DATA);
    }
    return err;
}

// The slug asked for, or the active list's. Called with the lock held.
static const p3a_pin_list_t *list_of(const char *slug)
{
    return slug != NULL ? p3a_pin_table_find(s_table, slug) : p3a_pin_table_active(s_table);
}

esp_err_t p3a_pins_start(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_table = psram_calloc(1, sizeof(*s_table));
    if (s_lock == NULL || s_table == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char path[P3A_FS_PATH_MAX];
    esp_err_t err = table_path(path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    const p3a_frame_expect_t expect = {.schema = P3A_FRAME_SCHEMA_PIN_TABLE,
                                       .version = P3A_PIN_TABLE_VERSION,
                                       .record_size = sizeof(p3a_pin_list_t),
                                       .max_count = P3A_PINS_MAX_LISTS};
    void *lists = NULL;
    uint32_t count = 0;
    err = p3a_frame_load(path, &expect, &lists, &count, NULL);
    if (err == ESP_OK) {
        err = p3a_pin_table_set(s_table, lists, count);
    }
    free(lists);
    if (err != ESP_OK && err != (esp_err_t)P3A_ERR_MISSING) {
        // Left as it is on the card. A list made later writes a new table
        // and the old file stays beside it as ".bak".
        ESP_LOGE(TAG, "the table of pin lists could not be read: %s", p3a_err_to_name(err));
        p3a_pin_table_init(s_table);
        return err;
    }
    // What the lists keep. One list at a time through one index.
    p3a_index_t index = {0};
    if (s_table->count > 0 && p3a_index_create(&index, P3A_PIN_LIST_MAX) == ESP_OK) {
        for (uint32_t i = 0; i < s_table->count; i++) {
            const esp_err_t loaded = list_load(s_table->lists[i].slug, &index);
            if (loaded != ESP_OK) {
                ESP_LOGW(TAG, "list %s could not be read: %s", s_table->lists[i].slug, p3a_err_to_name(loaded));
                continue;
            }
            for (uint32_t k = 0; k < index.count; k++) {
                p3a_pin_keep_add(&s_keep, index.records[k].blob_ref);
            }
            s_counts[i] = index.count;
        }
        p3a_index_destroy(&index);
    }
    if (s_table->count == 0 && !p3a_storage_is_handed_over()) {
        char slug[P3A_PIN_SLUG_LEN];
        if (p3a_pin_table_create(s_table, FIRST_LIST_NAME, 0, slug) == ESP_OK) {
            const esp_err_t saved = table_save();
            if (saved != ESP_OK) {
                ESP_LOGW(TAG, "the first pin list could not be saved: %s", p3a_err_to_name(saved));
            }
        }
    }
    ESP_LOGI(TAG, "%u lists, %u pins kept", (unsigned)s_table->count, (unsigned)s_keep.count);
    return ESP_OK;
}

esp_err_t p3a_pins_lists(p3a_pin_list_info_t *out, size_t capacity, size_t *count)
{
    *count = 0;
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    for (uint32_t i = 0; i < s_table->count && *count < capacity; i++) {
        p3a_pin_list_info_t *info = &out[(*count)++];
        memset(info, 0, sizeof(*info));
        memcpy(info->slug, s_table->lists[i].slug, sizeof(info->slug));
        memcpy(info->name, s_table->lists[i].name, sizeof(info->name));
        info->active = s_table->lists[i].active != 0;
        info->count = s_counts[i];
    }
    UNLOCK();
    return ESP_OK;
}

// Changes the table under the lock and saves it; on a failed save the
// change is taken back.
static esp_err_t change_table(esp_err_t (*change)(void *arg), void *arg)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    p3a_pin_table_t *before = psram_malloc(sizeof(*before));
    if (before == NULL) {
        return ESP_ERR_NO_MEM;
    }
    LOCK();
    *before = *s_table;
    esp_err_t err = change(arg);
    if (err == ESP_OK) {
        err = table_save();
        if (err != ESP_OK) {
            *s_table = *before;
        } else {
            // The counts go where their lists went: a list made is at the
            // end with none, a list deleted closes the gap.
            uint32_t counts[P3A_PINS_MAX_LISTS] = {0};
            for (uint32_t i = 0; i < s_table->count; i++) {
                for (uint32_t k = 0; k < before->count; k++) {
                    if (memcmp(s_table->lists[i].slug, before->lists[k].slug, P3A_PIN_SLUG_LEN) == 0) {
                        counts[i] = s_counts[k];
                    }
                }
            }
            memcpy(s_counts, counts, sizeof(s_counts));
            s_revision++;
        }
    }
    UNLOCK();
    free(before);
    if (err == ESP_OK) {
        p3a_event_emit(P3A_EVENT_PINS_CHANGED);
    }
    return err;
}

typedef struct {
    const char *slug;
    const char *name;
    char *out_slug;
} change_t;

static esp_err_t do_create(void *arg)
{
    change_t *c = arg;
    return p3a_pin_table_create(s_table, c->name, (uint32_t)esp_timer_get_time(), c->out_slug);
}

static esp_err_t do_rename(void *arg)
{
    change_t *c = arg;
    return p3a_pin_table_rename(s_table, c->slug, c->name);
}

static esp_err_t do_set_active(void *arg)
{
    change_t *c = arg;
    return p3a_pin_table_set_active(s_table, c->slug);
}

static esp_err_t do_delete(void *arg)
{
    change_t *c = arg;
    return p3a_pin_table_delete(s_table, c->slug);
}

esp_err_t p3a_pins_create(const char *name, char slug[P3A_PIN_SLUG_LEN])
{
    change_t c = {.name = name, .out_slug = slug};
    return change_table(do_create, &c);
}

esp_err_t p3a_pins_rename(const char *slug, const char *name)
{
    change_t c = {.slug = slug, .name = name};
    return change_table(do_rename, &c);
}

esp_err_t p3a_pins_set_active(const char *slug)
{
    change_t c = {.slug = slug};
    return change_table(do_set_active, &c);
}

esp_err_t p3a_pins_delete(const char *slug)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Its pins stop keeping their blobs. Read before the table forgets the list.
    p3a_index_t index = {0};
    esp_err_t err = p3a_index_create(&index, P3A_PIN_LIST_MAX);
    if (err != ESP_OK) {
        return err;
    }
    const bool had_pins = p3a_pin_slug_ok(slug) && list_load(slug, &index) == ESP_OK;
    change_t c = {.slug = slug};
    err = change_table(do_delete, &c);
    if (err == ESP_OK) {
        LOCK();
        for (uint32_t k = 0; had_pins && k < index.count; k++) {
            p3a_pin_keep_remove(&s_keep, index.records[k].blob_ref);
        }
        UNLOCK();
        // The person asked for the list to go: its file goes with it.
        char path[P3A_FS_PATH_MAX];
        if (list_path(slug, path, sizeof(path)) == ESP_OK) {
            unlink(path);
        }
    }
    p3a_index_destroy(&index);
    return err;
}

// Loads a list, hands it to `change`, and saves it if `change` says so.
static esp_err_t change_list(const char *slug, const p3a_record_t *record, bool pin)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record == NULL || p3a_record_check(record) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pin && ((record->flags & P3A_RECORD_IN_PLACE) != 0 || record->source == P3A_SOURCE_LOCAL ||
                !p3a_record_has_blob(record))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    p3a_index_t index = {0};
    esp_err_t err = p3a_index_create(&index, P3A_PIN_LIST_MAX);
    if (err != ESP_OK) {
        return err;
    }
    LOCK();
    const p3a_pin_list_t *list = list_of(slug);
    if (list == NULL) {
        err = P3A_ERR_MISSING;
    } else {
        err = list_load(list->slug, &index);
    }
    if (err == ESP_OK) {
        const int32_t at = p3a_index_find_record(&index, record);
        uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
        if (pin) {
            memcpy(ref, record->blob_ref, sizeof(ref));
            err = at >= 0 ? ESP_ERR_INVALID_STATE : p3a_index_append(&index, record, NULL);
        } else if (at < 0) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            memcpy(ref, index.records[at].blob_ref, sizeof(ref));
            err = p3a_index_remove(&index, (uint32_t)at);
        }
        if (err == ESP_OK) {
            err = list_save(list->slug, &index);
        }
        if (err == ESP_OK) {
            if (pin) {
                p3a_pin_keep_add(&s_keep, ref);
            } else {
                p3a_pin_keep_remove(&s_keep, ref);
            }
            s_counts[list - s_table->lists] = index.count;
            s_revision++;
        }
    }
    UNLOCK();
    p3a_index_destroy(&index);
    if (err == ESP_OK) {
        p3a_event_emit(P3A_EVENT_PINS_CHANGED);
    }
    return err;
}

esp_err_t p3a_pins_pin(const char *slug, const p3a_record_t *record)
{
    return change_list(slug, record, true);
}

esp_err_t p3a_pins_unpin(const char *slug, const p3a_record_t *record)
{
    return change_list(slug, record, false);
}

bool p3a_pins_is_pinned(const char *slug, const p3a_record_t *record)
{
    if (s_lock == NULL || record == NULL) {
        return false;
    }
    p3a_index_t index = {0};
    if (p3a_index_create(&index, P3A_PIN_LIST_MAX) != ESP_OK) {
        return false;
    }
    LOCK();
    const p3a_pin_list_t *list = list_of(slug);
    const bool pinned = list != NULL && list_load(list->slug, &index) == ESP_OK && p3a_index_find_record(&index, record) >= 0;
    UNLOCK();
    p3a_index_destroy(&index);
    return pinned;
}

esp_err_t p3a_pins_load(const char *slug, p3a_index_t *index)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCK();
    const p3a_pin_list_t *list = list_of(slug);
    const esp_err_t err = list != NULL ? list_load(list->slug, index) : (esp_err_t)P3A_ERR_MISSING;
    UNLOCK();
    return err;
}

bool p3a_pins_keeps(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    if (s_lock == NULL) {
        return false;
    }
    LOCK();
    const bool kept = p3a_pin_keep_has(&s_keep, ref);
    UNLOCK();
    return kept;
}

uint32_t p3a_pins_revision(void)
{
    return s_revision;
}
