// SPDX-License-Identifier: Apache-2.0
#include "playset_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "p3a_err.h"
#include "p3a_frame.h"
#include "psram_alloc.h"
#include "sd_path.h"

static const char *TAG = "playsets";

#define PATH_MAX_LEN 192
#define ACTIVE_FILE  "active.p3af"

static const p3a_frame_expect_t s_expect = {.schema = P3A_FRAME_SCHEMA_PLAYSET,
                                            .version = PLAYSET_VERSION,
                                            .record_size = PLAYSET_RECORD_SIZE,
                                            .max_count = PLAYSET_MAX_CHANNELS + 1};

static esp_err_t folder(char *out, size_t len)
{
    return sd_path_get_subdir("playsets", out, len);
}

// The file a name is kept in. Names may hold characters FAT does not take
// and differ only by case, so the file is named by a hash of the name.
esp_err_t playset_store_path(const char *name, char *out, size_t len)
{
    char dir[128];
    esp_err_t err = folder(dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h = (h ^ *p) * 1099511628211ull;
    }
    const int n = snprintf(out, len, "%s/%08x%08x.p3af", dir, (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFu));
    return (n > 0 && (size_t)n < len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t active_path(char *out, size_t len)
{
    char dir[128];
    esp_err_t err = folder(dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    const int n = snprintf(out, len, "%s/%s", dir, ACTIVE_FILE);
    return (n > 0 && (size_t)n < len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t save_to(const char *path, const playset_t *playset)
{
    void *records = psram_malloc((PLAYSET_MAX_CHANNELS + 1) * PLAYSET_RECORD_SIZE);
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const uint32_t count = playset_pack(playset, records);
    const esp_err_t err = p3a_frame_save(path, P3A_FRAME_SCHEMA_PLAYSET, PLAYSET_VERSION, PLAYSET_RECORD_SIZE, count,
                                         P3A_FRAME_USER_DATA, records);
    free(records);
    return err;
}

static esp_err_t load_from(const char *path, playset_t *out)
{
    void *records = NULL;
    uint32_t count = 0;
    esp_err_t err = p3a_frame_load(path, &s_expect, &records, &count, NULL);
    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }
    err = playset_unpack(records, count, out);
    free(records);
    return err;
}

static bool is_playset_file(const char *name)
{
    const size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".p3af") == 0 && strcmp(name, ACTIVE_FILE) != 0;
}

esp_err_t playset_store_list(playset_store_entry_t *out, uint32_t max, uint32_t *count, uint32_t *unreadable)
{
    *count = 0;
    if (unreadable) {
        *unreadable = 0;
    }
    char dir[128];
    esp_err_t err = folder(dir, sizeof(dir));
    if (err != ESP_OK) {
        return err;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;  // no folder yet: nothing saved
    }
    playset_t *playset = psram_malloc(sizeof(playset_t));
    if (playset == NULL) {
        closedir(d);
        return ESP_ERR_NO_MEM;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!is_playset_file(entry->d_name)) {
            continue;
        }
        char path[PATH_MAX_LEN];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        if (load_from(path, playset) != ESP_OK || playset->name[0] == '\0') {
            ESP_LOGW(TAG, "%s cannot be read as a playset; left in place", entry->d_name);
            if (unreadable) {
                (*unreadable)++;
            }
            continue;
        }
        if (*count < max) {
            memcpy(out[*count].name, playset->name, sizeof(out[*count].name));
            out[*count].channel_count = playset->count;
            (*count)++;
        }
    }
    free(playset);
    closedir(d);
    return ESP_OK;
}

// How many files in the folder look like saved playsets, readable or not.
static uint32_t files_present(void)
{
    char dir[128];
    uint32_t n = 0;
    if (folder(dir, sizeof(dir)) != ESP_OK) {
        return 0;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        n += is_playset_file(entry->d_name) ? 1 : 0;
    }
    closedir(d);
    return n;
}

esp_err_t playset_store_save(const playset_t *playset)
{
    if (playset == NULL || playset->name[0] == '\0' || playset_check(playset, NULL, 0) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[PATH_MAX_LEN];
    esp_err_t err = playset_store_path(playset->name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        // Replacing: only a playset of this very name, and only if it reads.
        playset_t *held = psram_malloc(sizeof(playset_t));
        if (held == NULL) {
            return ESP_ERR_NO_MEM;
        }
        const bool same = load_from(path, held) == ESP_OK && strcmp(held->name, playset->name) == 0;
        free(held);
        if (!same) {
            ESP_LOGW(TAG, "the file for \"%s\" holds something else; not overwritten", playset->name);
            return ESP_ERR_INVALID_STATE;
        }
    } else if (files_present() >= PLAYSET_STORE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    return save_to(path, playset);
}

esp_err_t playset_store_load(const char *name, playset_t *out)
{
    char path[PATH_MAX_LEN];
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = playset_store_path(name, path, sizeof(path));
    if (err == ESP_OK) {
        err = load_from(path, out);
    }
    if (err == ESP_OK && strcmp(out->name, name) != 0) {
        memset(out, 0, sizeof(*out));
        return P3A_ERR_MISSING;  // another name with the same hash lives there
    }
    return err;
}

esp_err_t playset_store_delete(const char *name)
{
    char path[PATH_MAX_LEN];
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = playset_store_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    if (unlink(path) != 0) {
        return errno == ENOENT ? (esp_err_t)P3A_ERR_MISSING : ESP_FAIL;
    }
    char bak[PATH_MAX_LEN + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    unlink(bak);  // the copy kept from the last save goes with it
    return ESP_OK;
}

esp_err_t playset_store_save_active(const playset_t *playset)
{
    char path[PATH_MAX_LEN];
    if (playset == NULL || playset_check(playset, NULL, 0) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = active_path(path, sizeof(path));
    return err == ESP_OK ? save_to(path, playset) : err;
}

esp_err_t playset_store_load_active(playset_t *out)
{
    char path[PATH_MAX_LEN];
    esp_err_t err = active_path(path, sizeof(path));
    return err == ESP_OK ? load_from(path, out) : err;
}
