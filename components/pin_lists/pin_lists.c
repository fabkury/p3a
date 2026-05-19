// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file pin_lists.c
 * @brief Orchestration + cross-file helpers for the pin_lists module
 */

#include "pin_lists.h"
#include "pin_lists_internal.h"
#include "sd_path.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char *TAG = "pin_lists";

/* ------------------------------------------------------------------------- */
/*  Module state                                                             */
/* ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_mutex = NULL;
static bool              s_initialized = false;
static char              s_active_slug[PIN_LIST_SLUG_LEN] = {0};

#define LOCK()    do { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); } while (0)
#define UNLOCK()  do { if (s_mutex) xSemaphoreGive(s_mutex); } while (0)

/* Background GC: rmtree's `.deleting-*` tombstones outside the pin_lists mutex.
   Woken by pin_lists_delete (immediate cleanup) and pin_lists_gc_kick (typically
   called from the playset-wide refresh completion path to reclaim tombstones
   that survived a power loss). */
static SemaphoreHandle_t s_gc_sem = NULL;
static TaskHandle_t      s_gc_task = NULL;
static StackType_t      *s_gc_stack = NULL;
static StaticTask_t      s_gc_task_buffer;
static bool              s_gc_stack_in_psram = false;

#define PL_GC_STACK_SIZE      (12 * 1024)
#define PL_GC_TASK_PRIORITY   3
#define PL_GC_TASK_NAME       "pin_lists_gc"
#define PL_TOMBSTONE_PREFIX   ".deleting-"

/* ------------------------------------------------------------------------- */
/*  Path builders                                                            */
/* ------------------------------------------------------------------------- */

esp_err_t pl_paths_root(char *out, size_t out_len)
{
    return sd_path_get_pinned(out, out_len);
}

esp_err_t pl_paths_state(char *out, size_t out_len)
{
    char root[160];
    esp_err_t err = pl_paths_root(root, sizeof(root));
    if (err != ESP_OK) return err;
    int n = snprintf(out, out_len, "%s/state.bin", root);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t pl_paths_lists_root(char *out, size_t out_len)
{
    char root[160];
    esp_err_t err = pl_paths_root(root, sizeof(root));
    if (err != ESP_OK) return err;
    int n = snprintf(out, out_len, "%s/lists", root);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t pl_paths_list_dir(const char *slug, char *out, size_t out_len)
{
    if (!pl_slug_is_valid(slug)) return ESP_ERR_INVALID_ARG;
    char lists_root[180];
    esp_err_t err = pl_paths_lists_root(lists_root, sizeof(lists_root));
    if (err != ESP_OK) return err;
    int n = snprintf(out, out_len, "%s/%s", lists_root, slug);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t pl_paths_manifest(const char *slug, char *out, size_t out_len)
{
    char dir[200];
    esp_err_t err = pl_paths_list_dir(slug, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    int n = snprintf(out, out_len, "%s/manifest.json", dir);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t pl_paths_order(const char *slug, char *out, size_t out_len)
{
    char dir[200];
    esp_err_t err = pl_paths_list_dir(slug, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    int n = snprintf(out, out_len, "%s/order.bin", dir);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static const char *source_short(pinned_source_t src)
{
    switch (src) {
        case PINNED_SOURCE_MAKAPIX:     return "mkpx";
        case PINNED_SOURCE_GIPHY:       return "giph";
        case PINNED_SOURCE_INSTITUTION: return "inst";
        default:                        return "unkn";
    }
}

esp_err_t pl_paths_entry(const char *slug, pinned_source_t src, const char *source_id,
                         char *out, size_t out_len)
{
    if (!source_id || source_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    char dir[200];
    esp_err_t err = pl_paths_list_dir(slug, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    char hash_hex[13];
    pl_source_id_hash(src, source_id, hash_hex);
    int n = snprintf(out, out_len, "%s/entries/%s_%s.bin",
                     dir, source_short(src), hash_hex);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ------------------------------------------------------------------------- */
/*  Slug helpers                                                             */
/* ------------------------------------------------------------------------- */

bool pl_slug_is_valid(const char *slug)
{
    if (!slug) return false;
    size_t n = strlen(slug);
    if (n != PIN_LIST_SLUG_LEN - 1) return false;
    for (size_t i = 0; i < n; i++) {
        char c = slug[i];
        bool is_digit = (c >= '0' && c <= '9');
        bool is_lower_hex = (c >= 'a' && c <= 'f');
        if (!is_digit && !is_lower_hex) return false;
    }
    return true;
}

void pl_slug_generate(char out_slug[PIN_LIST_SLUG_LEN])
{
    uint32_t r = esp_random();
    snprintf(out_slug, PIN_LIST_SLUG_LEN, "%08lx", (unsigned long)r);
}

/* ------------------------------------------------------------------------- */
/*  Source-id hashing (for entry filename component)                         */
/* ------------------------------------------------------------------------- */

void pl_source_id_hash(pinned_source_t src, const char *source_id,
                       char out_hex[13])
{
    /* DJB2 with the source byte folded in. 48 bits -> 12 hex chars. */
    uint64_t h = 5381;
    h = ((h << 5) + h) ^ (uint8_t)src;
    for (const unsigned char *p = (const unsigned char *)source_id; *p; p++) {
        h = ((h << 5) + h) ^ *p;
    }
    uint64_t mask = (1ULL << 48) - 1;
    snprintf(out_hex, 13, "%012llx", (unsigned long long)(h & mask));
}

/* ------------------------------------------------------------------------- */
/*  CRC32                                                                    */
/* ------------------------------------------------------------------------- */

uint32_t pl_crc32(const uint8_t *data, size_t len)
{
    /* Matches playset_store.c convention: seed 0, no final XOR. */
    return esp_rom_crc32_le(0, data, len);
}

/* ------------------------------------------------------------------------- */
/*  Recursive directory delete                                               */
/* ------------------------------------------------------------------------- */

esp_err_t pl_rmtree(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) return ESP_OK;
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0) ? ESP_OK : ESP_FAIL;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed: %s (%s)", path, strerror(errno));
        return ESP_FAIL;
    }
    esp_err_t result = ESP_OK;
    struct dirent *de;
    char child[256];
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) { result = ESP_ERR_INVALID_SIZE; break; }
        esp_err_t e = pl_rmtree(child);
        if (e != ESP_OK) { result = e; break; }
    }
    closedir(dir);

    if (result == ESP_OK && rmdir(path) != 0) {
        ESP_LOGW(TAG, "rmdir failed: %s (%s)", path, strerror(errno));
        result = ESP_FAIL;
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/*  Tombstone helpers (background GC)                                        */
/* ------------------------------------------------------------------------- */

/* Build a tombstone path `{lists_root}/.deleting-{slug}-{8hex}` with bounds
   checking. The 8-hex suffix is from esp_random() so back-to-back deletes of
   the same slug (after re-creation) cannot collide on disk. */
static esp_err_t pl_paths_tombstone(const char *slug, char *out, size_t out_len)
{
    char lists_root[180];
    esp_err_t err = pl_paths_lists_root(lists_root, sizeof(lists_root));
    if (err != ESP_OK) return err;
    uint32_t rnd = esp_random();
    int n = snprintf(out, out_len, "%s/" PL_TOMBSTONE_PREFIX "%s-%08lx",
                     lists_root, slug, (unsigned long)rnd);
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* Find the first tombstone in lists_root. Returns true and fills `out` with
   the absolute path; returns false if none exist. Each call reopens the dir
   so no handle survives concurrent renames. */
static bool next_tombstone(const char *lists_root, char *out, size_t out_len)
{
    DIR *d = opendir(lists_root);
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, PL_TOMBSTONE_PREFIX,
                    sizeof(PL_TOMBSTONE_PREFIX) - 1) != 0) continue;
        int n = snprintf(out, out_len, "%s/%s", lists_root, de->d_name);
        if (n > 0 && (size_t)n < out_len) found = true;
        break;
    }
    closedir(d);
    return found;
}

/* Background worker: drains all tombstones each time it wakes. Holds NO
   pin_lists mutex while rmtree'ing — tombstones are unreachable to public
   APIs because their names fail pl_slug_is_valid(). */
static void pl_gc_task(void *arg)
{
    (void)arg;
    char lists_root[180];
    char tomb[256];
    for (;;) {
        xSemaphoreTake(s_gc_sem, portMAX_DELAY);
        if (pl_paths_lists_root(lists_root, sizeof(lists_root)) != ESP_OK) continue;
        while (next_tombstone(lists_root, tomb, sizeof(tomb))) {
            ESP_LOGI(TAG, "gc start: %s", tomb);
            int64_t t0 = esp_timer_get_time();
            esp_err_t err = pl_rmtree(tomb);
            int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
            ESP_LOGI(TAG, "gc done : %s (%lld ms, %s)",
                     tomb, (long long)elapsed_ms,
                     err == ESP_OK ? "ok" : esp_err_to_name(err));
        }
    }
}

void pin_lists_gc_kick(void)
{
    if (s_gc_sem) xSemaphoreGive(s_gc_sem);
}

/* ------------------------------------------------------------------------- */
/*  Forward declarations for orchestration                                   */
/* ------------------------------------------------------------------------- */

static esp_err_t ensure_list_skeleton(const char *slug, const char *name);
static esp_err_t bootstrap_default_list_locked(char out_slug[PIN_LIST_SLUG_LEN]);
static esp_err_t persist_active_locked(const char *slug);
static int       count_lists_locked(void);

/* ------------------------------------------------------------------------- */
/*  pin_lists_init                                                           */
/* ------------------------------------------------------------------------- */

esp_err_t pin_lists_init(void)
{
    if (s_initialized) return ESP_OK;

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    LOCK();

    /* Ensure the on-disk skeleton exists. */
    char root[160], lists_root[180];
    if (pl_paths_root(root, sizeof(root)) != ESP_OK ||
        pl_paths_lists_root(lists_root, sizeof(lists_root)) != ESP_OK) {
        UNLOCK();
        return ESP_FAIL;
    }
    if (mkdir(root, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", root, strerror(errno));
        UNLOCK();
        return ESP_FAIL;
    }
    if (mkdir(lists_root, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", lists_root, strerror(errno));
        UNLOCK();
        return ESP_FAIL;
    }

    /* Load or bootstrap state.bin. */
    pinned_state_t state = {0};
    esp_err_t err = pl_state_load(&state);
    if (err == ESP_OK && pl_slug_is_valid(state.active_slug)) {
        /* Verify the active slug refers to an existing list. */
        char list_dir[220];
        struct stat st;
        if (pl_paths_list_dir(state.active_slug, list_dir, sizeof(list_dir)) == ESP_OK &&
            stat(list_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            strlcpy(s_active_slug, state.active_slug, sizeof(s_active_slug));
            ESP_LOGI(TAG, "active list: %s", s_active_slug);
        } else {
            ESP_LOGW(TAG, "state.bin references missing list '%s'; rebootstrapping", state.active_slug);
            err = ESP_ERR_NOT_FOUND;
        }
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "state.bin has malformed active_slug; rebootstrapping");
        err = ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK) {
        /* state.bin missing or unrecoverable. Prefer promoting an existing list
           over creating a fresh one — orphaning old lists with their artwork is
           more destructive than picking a slug we can see on disk. */
        char lists_root[180];
        char promoted[PIN_LIST_SLUG_LEN] = {0};
        if (pl_paths_lists_root(lists_root, sizeof(lists_root)) == ESP_OK) {
            DIR *d = opendir(lists_root);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (!pl_slug_is_valid(de->d_name)) continue;
                    if (promoted[0] == '\0' || strcmp(de->d_name, promoted) < 0) {
                        strlcpy(promoted, de->d_name, sizeof(promoted));
                    }
                }
                closedir(d);
            }
        }
        if (promoted[0] != '\0') {
            ESP_LOGW(TAG, "state.bin missing/corrupt; promoting existing list '%s' to active",
                     promoted);
            err = persist_active_locked(promoted);
            if (err != ESP_OK) { UNLOCK(); return err; }
            strlcpy(s_active_slug, promoted, sizeof(s_active_slug));
        } else {
            char fresh[PIN_LIST_SLUG_LEN];
            esp_err_t b = bootstrap_default_list_locked(fresh);
            if (b != ESP_OK) { UNLOCK(); return b; }
            strlcpy(s_active_slug, fresh, sizeof(s_active_slug));
        }
    }

    s_initialized = true;
    int total = count_lists_locked();
    ESP_LOGI(TAG, "init ok, lists=%d active=%s", total, s_active_slug);
    UNLOCK();

    /* Spawn the background GC worker. Failure is non-fatal: deletes still
       rename to a tombstone, but on-disk cleanup won't run until the next
       boot (where init is retried). Not kicked here — the boot path stays
       fast; the playset-wide refresh completion path calls gc_kick to
       reclaim any tombstones left over from a previous power loss. */
    s_gc_sem = xSemaphoreCreateBinary();
    if (!s_gc_sem) {
        ESP_LOGE(TAG, "gc sem create failed; tombstone cleanup disabled");
        return ESP_OK;
    }
    s_gc_stack = heap_caps_malloc(PL_GC_STACK_SIZE * sizeof(StackType_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_gc_stack) {
        s_gc_stack_in_psram = true;
        s_gc_task = xTaskCreateStaticPinnedToCore(
            pl_gc_task, PL_GC_TASK_NAME, PL_GC_STACK_SIZE, NULL,
            PL_GC_TASK_PRIORITY, s_gc_stack, &s_gc_task_buffer, 0);
        if (s_gc_task) {
            ESP_LOGI(TAG, "gc task using PSRAM stack (Core 0)");
            return ESP_OK;
        }
        heap_caps_free(s_gc_stack);
        s_gc_stack = NULL;
        s_gc_stack_in_psram = false;
    }
    ESP_LOGW(TAG, "gc PSRAM stack unavailable, using internal RAM");
    if (xTaskCreatePinnedToCore(pl_gc_task, PL_GC_TASK_NAME, PL_GC_STACK_SIZE,
                                NULL, PL_GC_TASK_PRIORITY, &s_gc_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "gc task create failed; tombstone cleanup disabled");
        vSemaphoreDelete(s_gc_sem);
        s_gc_sem = NULL;
        s_gc_task = NULL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Internal helpers (orchestration)                                         */
/* ------------------------------------------------------------------------- */

static uint32_t now_unix_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

static int count_lists_locked(void)
{
    char lists_root[180];
    if (pl_paths_lists_root(lists_root, sizeof(lists_root)) != ESP_OK) return 0;
    DIR *d = opendir(lists_root);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (pl_slug_is_valid(de->d_name)) n++;
    }
    closedir(d);
    return n;
}

static esp_err_t ensure_list_skeleton(const char *slug, const char *name)
{
    if (!pl_slug_is_valid(slug) || !name) return ESP_ERR_INVALID_ARG;

    char dir[220];
    esp_err_t err = pl_paths_list_dir(slug, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", dir, strerror(errno));
        return ESP_FAIL;
    }

    /* Create entries/ subdir. */
    char entries_dir[256];
    int n = snprintf(entries_dir, sizeof(entries_dir), "%s/entries", dir);
    if (n > 0 && (size_t)n < sizeof(entries_dir)) {
        if (mkdir(entries_dir, 0755) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed: %s", entries_dir, strerror(errno));
        }
    }

    /* Write the initial manifest. */
    pl_manifest_t m = {
        .version = PINNED_FORMAT_VERSION,
        .created_at = now_unix_seconds(),
        .next_post_id = 1,
        .count_cache = 0,
    };
    strlcpy(m.slug, slug, sizeof(m.slug));
    strlcpy(m.name, name, sizeof(m.name));
    err = pl_manifest_save(slug, &m);
    if (err != ESP_OK) return err;

    /* Write an empty order.bin. */
    err = pl_order_replace(slug, NULL, 0);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

static esp_err_t persist_active_locked(const char *slug)
{
    pinned_state_t state = {
        .magic = PINNED_STATE_MAGIC,
        .version = PINNED_FORMAT_VERSION,
    };
    strlcpy(state.active_slug, slug, sizeof(state.active_slug));
    return pl_state_save(&state);
}

static esp_err_t bootstrap_default_list_locked(char out_slug[PIN_LIST_SLUG_LEN])
{
    char slug[PIN_LIST_SLUG_LEN];
    /* Pick a fresh slug; retry on rare collision with an existing directory. */
    for (int attempt = 0; attempt < 8; attempt++) {
        pl_slug_generate(slug);
        char dir[220];
        if (pl_paths_list_dir(slug, dir, sizeof(dir)) != ESP_OK) continue;
        struct stat st;
        if (stat(dir, &st) != 0) break;
    }
    esp_err_t err = ensure_list_skeleton(slug, "My Pins");
    if (err != ESP_OK) return err;
    err = persist_active_locked(slug);
    if (err != ESP_OK) return err;
    strlcpy(out_slug, slug, PIN_LIST_SLUG_LEN);
    ESP_LOGI(TAG, "bootstrapped default list slug=%s name='My Pins'", slug);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Active list pointer                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t pin_lists_get_active(char out_slug[PIN_LIST_SLUG_LEN])
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!out_slug) return ESP_ERR_INVALID_ARG;
    LOCK();
    strlcpy(out_slug, s_active_slug, PIN_LIST_SLUG_LEN);
    UNLOCK();
    return ESP_OK;
}

esp_err_t pin_lists_set_active(const char *slug)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!pl_slug_is_valid(slug)) return ESP_ERR_INVALID_ARG;

    LOCK();
    char dir[220];
    struct stat st;
    if (pl_paths_list_dir(slug, dir, sizeof(dir)) != ESP_OK ||
        stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = persist_active_locked(slug);
    if (err == ESP_OK) {
        strlcpy(s_active_slug, slug, sizeof(s_active_slug));
        ESP_LOGI(TAG, "active list -> %s", slug);
    }
    UNLOCK();
    return err;
}

/* ------------------------------------------------------------------------- */
/*  List management                                                          */
/* ------------------------------------------------------------------------- */

esp_err_t pin_lists_create(const char *name, char out_slug[PIN_LIST_SLUG_LEN])
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!name || name[0] == '\0' || !out_slug) return ESP_ERR_INVALID_ARG;

    LOCK();
    if (count_lists_locked() >= PIN_LISTS_MAX_LISTS) {
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    char slug[PIN_LIST_SLUG_LEN];
    for (int attempt = 0; attempt < 8; attempt++) {
        pl_slug_generate(slug);
        char dir[220];
        if (pl_paths_list_dir(slug, dir, sizeof(dir)) != ESP_OK) continue;
        struct stat st;
        if (stat(dir, &st) != 0) break;
    }
    esp_err_t err = ensure_list_skeleton(slug, name);
    if (err == ESP_OK) {
        strlcpy(out_slug, slug, PIN_LIST_SLUG_LEN);
        ESP_LOGI(TAG, "created list slug=%s name='%s'", slug, name);
    }
    UNLOCK();
    return err;
}

esp_err_t pin_lists_rename(const char *slug, const char *new_name)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!pl_slug_is_valid(slug) || !new_name || new_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    LOCK();
    pl_manifest_t m;
    esp_err_t err = pl_manifest_load(slug, &m);
    if (err != ESP_OK) { UNLOCK(); return err; }
    strlcpy(m.name, new_name, sizeof(m.name));
    err = pl_manifest_save(slug, &m);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "renamed list %s -> '%s'", slug, new_name);
    }
    UNLOCK();
    return err;
}

esp_err_t pin_lists_delete(const char *slug)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!pl_slug_is_valid(slug)) return ESP_ERR_INVALID_ARG;

    LOCK();
    if (strcmp(slug, s_active_slug) == 0) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    char dir[220];
    esp_err_t err = pl_paths_list_dir(slug, dir, sizeof(dir));
    if (err != ESP_OK) { UNLOCK(); return err; }
    struct stat st;
    if (stat(dir, &st) != 0) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    /* Atomically rename the live dir to a hidden tombstone so it disappears
       from every enumeration (pl_slug_is_valid rejects the `.deleting-*`
       prefix). The recursive unlink happens later in the GC worker, outside
       the pin_lists mutex. */
    char tomb[256];
    err = pl_paths_tombstone(slug, tomb, sizeof(tomb));
    if (err != ESP_OK) { UNLOCK(); return err; }
    if (rename(dir, tomb) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: %s", dir, tomb, strerror(errno));
        UNLOCK();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted list %s (tombstone %s)", slug, tomb);

    /* If we somehow ended up with zero lists, recreate the default. The
       active-list guard above makes this unreachable in practice, but keep
       parity with prior behavior. */
    if (count_lists_locked() == 0) {
        char fresh[PIN_LIST_SLUG_LEN];
        bootstrap_default_list_locked(fresh);
        strlcpy(s_active_slug, fresh, sizeof(s_active_slug));
    }
    UNLOCK();

    /* Wake the GC. Safe if the worker failed to start at init — the rename
       has already removed the list from view; the bytes just linger. */
    if (s_gc_sem) xSemaphoreGive(s_gc_sem);
    return ESP_OK;
}

esp_err_t pin_lists_enumerate(pin_list_info_t *out, size_t cap, size_t *out_n)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!out_n) return ESP_ERR_INVALID_ARG;

    LOCK();
    char lists_root[180];
    if (pl_paths_lists_root(lists_root, sizeof(lists_root)) != ESP_OK) {
        UNLOCK();
        return ESP_FAIL;
    }
    DIR *d = opendir(lists_root);
    if (!d) {
        *out_n = 0;
        UNLOCK();
        return ESP_OK;
    }

    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!pl_slug_is_valid(de->d_name)) continue;
        if (out && n < cap) {
            pin_list_info_t *e = &out[n];
            memset(e, 0, sizeof(*e));
            strlcpy(e->slug, de->d_name, sizeof(e->slug));
            pl_manifest_t m;
            if (pl_manifest_load(de->d_name, &m) == ESP_OK) {
                strlcpy(e->name, m.name, sizeof(e->name));
                e->created_at = m.created_at;
                e->count = m.count_cache;
            } else {
                strlcpy(e->name, "(corrupt)", sizeof(e->name));
            }
            e->is_active = (strcmp(de->d_name, s_active_slug) == 0);
        }
        n++;
    }
    closedir(d);
    *out_n = n;
    UNLOCK();
    return ESP_OK;
}

esp_err_t pin_lists_get_info(const char *slug, pin_list_info_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!pl_slug_is_valid(slug) || !out) return ESP_ERR_INVALID_ARG;

    LOCK();
    pl_manifest_t m;
    esp_err_t err = pl_manifest_load(slug, &m);
    if (err != ESP_OK) { UNLOCK(); return err; }
    memset(out, 0, sizeof(*out));
    strlcpy(out->slug, m.slug, sizeof(out->slug));
    strlcpy(out->name, m.name, sizeof(out->name));
    out->created_at = m.created_at;
    out->count = m.count_cache;
    out->is_active = (strcmp(slug, s_active_slug) == 0);
    UNLOCK();
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/*  Per-list operations                                                      */
/* ------------------------------------------------------------------------- */

size_t pin_list_count(const char *slug)
{
    if (!s_initialized || !pl_slug_is_valid(slug)) return 0;
    LOCK();
    pl_manifest_t m;
    size_t n = (pl_manifest_load(slug, &m) == ESP_OK) ? m.count_cache : 0;
    UNLOCK();
    return n;
}

bool pin_list_is_pinned(const char *slug, pinned_source_t src, const char *source_id)
{
    if (!s_initialized || !source_id) return false;
    const char *target_slug = (slug && slug[0]) ? slug : s_active_slug;
    if (!pl_slug_is_valid(target_slug)) return false;
    LOCK();
    bool present = pl_entry_exists(target_slug, src, source_id);
    UNLOCK();
    return present;
}

esp_err_t pin_list_build_artwork_path(const char *slug, const pinned_order_entry_t *e,
                                      char *out, size_t out_len)
{
    if (!pl_slug_is_valid(slug) || !e || !out || out_len == 0) return ESP_ERR_INVALID_ARG;

    static const char *ext_str[] = { ".webp", ".gif", ".png", ".jpg" };
    int ext_idx = (e->extension <= 3) ? e->extension : 0;

    char dir[220];
    esp_err_t err = pl_paths_list_dir(slug, dir, sizeof(dir));
    if (err != ESP_OK) return err;

    int n;
    switch ((pinned_source_t)e->source) {
        case PINNED_SOURCE_MAKAPIX: {
            /* Convert UUID bytes back to canonical hyphenated string. */
            const uint8_t *u = e->makapix.storage_key_uuid;
            char uuid[40];
            snprintf(uuid, sizeof(uuid),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            n = snprintf(out, out_len, "%s/makapix/%s%s", dir, uuid, ext_str[ext_idx]);
            break;
        }
        case PINNED_SOURCE_GIPHY:
            n = snprintf(out, out_len, "%s/giphy/%.*s%s",
                         dir, PINNED_GIPHY_ID_MAX, e->giphy.giphy_id, ext_str[ext_idx]);
            break;
        case PINNED_SOURCE_INSTITUTION: {
            // iiif_key is stored in canonical form (matches the channel cache
            // and round-trips through art_institution_build_iiif_url for URL
            // reconstruction at playback time). HAM's URN carries colons,
            // which FAT/exFAT forbids in filenames; sanitize for the leaf
            // name only. Other museums' identifiers use only FAT-safe
            // characters, so the substitution is a no-op for them. See
            // sd_path_sanitize_filename's docstring.
            char safe_key[PINNED_IIIF_KEY_MAX + 1];
            sd_path_sanitize_filename(e->museum.iiif_key, safe_key, sizeof(safe_key));
            n = snprintf(out, out_len, "%s/museum/%u/%s%s",
                         dir, (unsigned)e->museum.museum_id,
                         safe_key, ext_str[ext_idx]);
            break;
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
    return (n > 0 && (size_t)n < out_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t pin_list_pin(const char *slug,
                       const pinned_order_entry_t *order_e,
                       const pinned_entry_file_t *file_e,
                       const char *src_artwork_path)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!order_e || !file_e || !src_artwork_path) return ESP_ERR_INVALID_ARG;
    const char *target_slug = (slug && slug[0]) ? slug : s_active_slug;
    if (!pl_slug_is_valid(target_slug)) return ESP_ERR_INVALID_ARG;

    LOCK();

    /* Cap check (load manifest once for next_post_id + count). */
    pl_manifest_t manifest;
    esp_err_t err = pl_manifest_load(target_slug, &manifest);
    if (err != ESP_OK) { UNLOCK(); return err; }
    if (manifest.version > PINNED_FORMAT_VERSION) {
        UNLOCK();
        return ESP_ERR_NOT_SUPPORTED;
    }

    pinned_source_t src = (pinned_source_t)file_e->source;
    const char *source_id = file_e->source_id;

    /* Dedup: if already pinned, return OK (idempotent). */
    if (pl_entry_exists(target_slug, src, source_id)) {
        UNLOCK();
        return ESP_OK;
    }
    if (manifest.count_cache >= PIN_LIST_MAX_ENTRIES) {
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }

    /* The caller has populated order_e->post_id with the ORIGINAL source
       post_id (Makapix server id, or DJB2 hash for Giphy/museum). pin_list_pin
       no longer mints a list-local id — manifest.next_post_id stays in the
       file for forward-compat but is unused. */
    pinned_order_entry_t order_copy = *order_e;
    pinned_entry_file_t file_copy = *file_e;
    if (file_copy.post_id == 0) file_copy.post_id = order_copy.post_id;
    file_copy.magic = PINNED_ENTRY_MAGIC;
    file_copy.version = PINNED_FORMAT_VERSION;

    /* Copy artwork into the list vault. */
    char dest_path[256];
    err = pin_list_build_artwork_path(target_slug, &order_copy, dest_path, sizeof(dest_path));
    if (err != ESP_OK) { UNLOCK(); return err; }
    err = pl_artwork_copy(src_artwork_path, dest_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pin: artwork copy failed src=%s dest=%s", src_artwork_path, dest_path);
        UNLOCK();
        return err;
    }

    /* Write the entry file. */
    err = pl_entry_write(target_slug, &file_copy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pin: entry write failed slug=%s", target_slug);
        unlink(dest_path);
        UNLOCK();
        return err;
    }

    /* Load current order, prepend new entry, rewrite atomically. */
    pinned_order_entry_t *all = NULL;
    size_t cur_n = 0;
    err = pl_order_read_all(target_slug, &all, &cur_n);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "pin: order read failed slug=%s err=%s", target_slug, esp_err_to_name(err));
        unlink(dest_path);
        pl_entry_delete(target_slug, src, source_id);
        UNLOCK();
        return err;
    }
    size_t new_n = cur_n + 1;
    pinned_order_entry_t *combined = malloc(new_n * sizeof(pinned_order_entry_t));
    if (!combined) {
        free(all);
        unlink(dest_path);
        pl_entry_delete(target_slug, src, source_id);
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    combined[0] = order_copy;
    if (cur_n) memcpy(&combined[1], all, cur_n * sizeof(pinned_order_entry_t));
    free(all);

    err = pl_order_replace(target_slug, combined, new_n);
    free(combined);
    if (err != ESP_OK) {
        unlink(dest_path);
        pl_entry_delete(target_slug, src, source_id);
        UNLOCK();
        return err;
    }

    /* Bump manifest count. next_post_id is preserved for forward-compat
       but no longer used. */
    manifest.count_cache = new_n;
    err = pl_manifest_save(target_slug, &manifest);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pin: manifest save failed slug=%s", target_slug);
    }

    ESP_LOGI(TAG, "pinned src=%d source_id=%s -> %s (post_id=%ld, total=%zu)",
             (int)src, source_id, target_slug, (long)order_copy.post_id, new_n);
    UNLOCK();
    return ESP_OK;
}

esp_err_t pin_list_unpin(const char *slug, pinned_source_t src, const char *source_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!source_id) return ESP_ERR_INVALID_ARG;
    const char *target_slug = (slug && slug[0]) ? slug : s_active_slug;
    if (!pl_slug_is_valid(target_slug)) return ESP_ERR_INVALID_ARG;

    LOCK();

    if (!pl_entry_exists(target_slug, src, source_id)) {
        UNLOCK();
        return ESP_OK;  /* idempotent */
    }

    /* Read the entry file to learn the artwork path before deleting. */
    pinned_entry_file_t entry;
    esp_err_t err = pl_entry_read(target_slug, src, source_id, &entry);
    if (err != ESP_OK) { UNLOCK(); return err; }

    /* Rewrite order.bin without the matching entry. */
    pinned_order_entry_t *all = NULL;
    size_t cur_n = 0;
    err = pl_order_read_all(target_slug, &all, &cur_n);
    if (err == ESP_ERR_NOT_FOUND) { cur_n = 0; all = NULL; err = ESP_OK; }
    if (err != ESP_OK) { UNLOCK(); return err; }

    size_t write_idx = 0;
    pinned_order_entry_t *kept = (cur_n > 0)
        ? malloc(cur_n * sizeof(pinned_order_entry_t)) : NULL;
    if (cur_n > 0 && !kept) {
        free(all);
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < cur_n; i++) {
        if (all[i].post_id == entry.post_id) continue;
        kept[write_idx++] = all[i];
    }
    free(all);

    err = pl_order_replace(target_slug, kept, write_idx);
    free(kept);
    if (err != ESP_OK) { UNLOCK(); return err; }

    /* Best-effort: remove the artwork file. */
    pinned_order_entry_t shim = {
        .source = entry.source,
        .extension = entry.extension,
    };
    if (entry.source == PINNED_SOURCE_INSTITUTION) {
        shim.museum.museum_id = entry.museum_id;
        /* entry.source_id is the composite "<museum_id>:<iiif_key>".
           Strip the prefix to recover just the iiif_key for path building. */
        const char *colon = strchr(entry.source_id, ':');
        const char *key = colon ? colon + 1 : entry.source_id;
        strlcpy(shim.museum.iiif_key, key, sizeof(shim.museum.iiif_key));
    } else if (entry.source == PINNED_SOURCE_GIPHY) {
        strlcpy(shim.giphy.giphy_id, entry.source_id, sizeof(shim.giphy.giphy_id));
    } else if (entry.source == PINNED_SOURCE_MAKAPIX) {
        /* source_id for makapix is the 36-char UUID; convert back to bytes. */
        const char *s = entry.source_id;
        for (int i = 0; i < 16 && *s; i++) {
            while (*s == '-') s++;
            if (!s[0] || !s[1]) break;
            unsigned int v;
            sscanf(s, "%2x", &v);
            shim.makapix.storage_key_uuid[i] = (uint8_t)v;
            s += 2;
        }
    }
    char art_path[256];
    if (pin_list_build_artwork_path(target_slug, &shim, art_path, sizeof(art_path)) == ESP_OK) {
        if (unlink(art_path) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "unpin: unlink %s failed: %s", art_path, strerror(errno));
        }
    }

    /* Delete the entry file. */
    pl_entry_delete(target_slug, src, source_id);

    /* Update manifest's cached count. */
    pl_manifest_t manifest;
    if (pl_manifest_load(target_slug, &manifest) == ESP_OK) {
        manifest.count_cache = write_idx;
        pl_manifest_save(target_slug, &manifest);
    }

    ESP_LOGI(TAG, "unpinned src=%d source_id=%s from %s (total=%zu)",
             (int)src, source_id, target_slug, write_idx);
    UNLOCK();
    return ESP_OK;
}

esp_err_t pin_list_list(const char *slug, size_t offset, size_t limit,
                        pinned_order_entry_t *out, size_t *out_n, size_t *out_total)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!out_n) return ESP_ERR_INVALID_ARG;
    const char *target_slug = (slug && slug[0]) ? slug : s_active_slug;
    if (!pl_slug_is_valid(target_slug)) return ESP_ERR_INVALID_ARG;

    LOCK();
    pinned_order_header_t hdr;
    esp_err_t err = pl_order_read_header(target_slug, &hdr);
    if (err == ESP_ERR_NOT_FOUND) {
        *out_n = 0;
        if (out_total) *out_total = 0;
        UNLOCK();
        return ESP_OK;
    }
    if (err != ESP_OK) { UNLOCK(); return err; }
    if (out_total) *out_total = hdr.count;
    if (offset >= hdr.count || limit == 0 || !out) {
        *out_n = 0;
        UNLOCK();
        return ESP_OK;
    }
    size_t remaining = hdr.count - offset;
    size_t take = (limit < remaining) ? limit : remaining;
    err = pl_order_read_range(target_slug, offset, take, out, out_n);
    UNLOCK();
    return err;
}

esp_err_t pin_list_get_entry(const char *slug, pinned_source_t src, const char *source_id,
                             pinned_entry_file_t *out)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!source_id || !out) return ESP_ERR_INVALID_ARG;
    const char *target_slug = (slug && slug[0]) ? slug : s_active_slug;
    if (!pl_slug_is_valid(target_slug)) return ESP_ERR_INVALID_ARG;
    LOCK();
    esp_err_t err = pl_entry_read(target_slug, src, source_id, out);
    UNLOCK();
    return err;
}

/* ------------------------------------------------------------------------- */
/*  Channel-cache integration                                                */
/* ------------------------------------------------------------------------- */

esp_err_t pin_lists_channel_load(const char *slug,
                                 pinned_order_entry_t **out_entries,
                                 size_t *out_count)
{
    if (!out_entries || !out_count) return ESP_ERR_INVALID_ARG;
    *out_entries = NULL;
    *out_count = 0;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    const char *target_slug = (slug && slug[0]) ? slug : s_active_slug;
    if (!pl_slug_is_valid(target_slug)) return ESP_ERR_INVALID_ARG;

    LOCK();
    esp_err_t err = pl_order_read_all(target_slug, out_entries, out_count);
    UNLOCK();
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;  /* empty list */
    return err;
}

void pin_lists_channel_unload(pinned_order_entry_t *entries)
{
    if (entries) free(entries);
}

/* ------------------------------------------------------------------------- */
/*  High-level source-specific helpers                                       */
/* ------------------------------------------------------------------------- */

static bool parse_uuid_to_bytes(const char *uuid_str, uint8_t out[16])
{
    if (!uuid_str) return false;
    const char *s = uuid_str;
    for (int i = 0; i < 16; i++) {
        while (*s == '-') s++;
        if (!s[0] || !s[1]) return false;
        unsigned int v;
        if (sscanf(s, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
        s += 2;
    }
    return true;
}

esp_err_t pin_lists_pin_makapix(const char *slug,
                                int32_t original_post_id,
                                const char *uuid_36chars,
                                uint8_t extension,
                                const char *title,
                                const char *creator,
                                uint32_t original_created_at,
                                const char *src_artwork_path)
{
    if (!uuid_36chars || !src_artwork_path) return ESP_ERR_INVALID_ARG;

    if (original_post_id <= 0) return ESP_ERR_INVALID_ARG;

    pinned_order_entry_t order = {0};
    order.post_id = original_post_id;
    order.source = PINNED_SOURCE_MAKAPIX;
    order.extension = extension;
    order.pinned_at = now_unix_seconds();
    if (!parse_uuid_to_bytes(uuid_36chars, order.makapix.storage_key_uuid)) {
        return ESP_ERR_INVALID_ARG;
    }

    pinned_entry_file_t file = {0};
    file.magic = PINNED_ENTRY_MAGIC;
    file.version = PINNED_FORMAT_VERSION;
    file.post_id = original_post_id;
    file.source = PINNED_SOURCE_MAKAPIX;
    file.extension = extension;
    file.pinned_at = order.pinned_at;
    file.original_post_id = original_post_id;
    file.original_created_at = original_created_at;
    strlcpy(file.source_id, uuid_36chars, sizeof(file.source_id));
    if (title)   strlcpy(file.title, title,   sizeof(file.title));
    if (creator) strlcpy(file.creator, creator, sizeof(file.creator));

    return pin_list_pin(slug, &order, &file, src_artwork_path);
}

esp_err_t pin_lists_unpin_makapix(const char *slug, const char *uuid_36chars)
{
    if (!uuid_36chars) return ESP_ERR_INVALID_ARG;
    return pin_list_unpin(slug, PINNED_SOURCE_MAKAPIX, uuid_36chars);
}

esp_err_t pin_lists_pin_giphy(const char *slug,
                              int32_t original_post_id,
                              const char *giphy_id,
                              uint8_t extension,
                              const char *title,
                              const char *creator,
                              uint32_t original_created_at,
                              const char *src_artwork_path)
{
    if (!giphy_id || !giphy_id[0] || !src_artwork_path) return ESP_ERR_INVALID_ARG;

    if (original_post_id <= 0) return ESP_ERR_INVALID_ARG;

    pinned_order_entry_t order = {0};
    order.post_id = original_post_id;
    order.source = PINNED_SOURCE_GIPHY;
    order.extension = extension;
    order.pinned_at = now_unix_seconds();
    strlcpy(order.giphy.giphy_id, giphy_id, sizeof(order.giphy.giphy_id));

    pinned_entry_file_t file = {0};
    file.magic = PINNED_ENTRY_MAGIC;
    file.version = PINNED_FORMAT_VERSION;
    file.post_id = original_post_id;
    file.source = PINNED_SOURCE_GIPHY;
    file.extension = extension;
    file.pinned_at = order.pinned_at;
    file.original_post_id = original_post_id;
    file.original_created_at = original_created_at;
    strlcpy(file.source_id, giphy_id, sizeof(file.source_id));
    if (title)   strlcpy(file.title, title,   sizeof(file.title));
    if (creator) strlcpy(file.creator, creator, sizeof(file.creator));

    return pin_list_pin(slug, &order, &file, src_artwork_path);
}

esp_err_t pin_lists_unpin_giphy(const char *slug, const char *giphy_id)
{
    if (!giphy_id || !giphy_id[0]) return ESP_ERR_INVALID_ARG;
    return pin_list_unpin(slug, PINNED_SOURCE_GIPHY, giphy_id);
}

/* Composite institution source_id used in entry filenames and dedup:
 *   "<museum_id>:<safe_iiif_key>"
 * The colon is fine here because the source_id is hashed into an opaque
 * filename component (pl_source_id_hash) — it never lands on the filesystem
 * verbatim, so FAT's reserved-character set doesn't apply. */
static void build_institution_source_id(uint16_t museum_id,
                                        const char *iiif_key_safe,
                                        char *out, size_t out_len)
{
    snprintf(out, out_len, "%u:%s", (unsigned)museum_id, iiif_key_safe);
}

esp_err_t pin_lists_pin_institution(const char *slug,
                                    int32_t original_post_id,
                                    uint16_t museum_id,
                                    const char *iiif_key_safe,
                                    uint8_t extension,
                                    const char *title,
                                    const char *creator,
                                    uint32_t original_created_at,
                                    const char *src_artwork_path)
{
    if (!iiif_key_safe || !iiif_key_safe[0] || !src_artwork_path) {
        return ESP_ERR_INVALID_ARG;
    }

    if (original_post_id <= 0) return ESP_ERR_INVALID_ARG;

    pinned_order_entry_t order = {0};
    order.post_id = original_post_id;
    order.source = PINNED_SOURCE_INSTITUTION;
    order.extension = extension;
    order.pinned_at = now_unix_seconds();
    order.museum.museum_id = museum_id;
    strlcpy(order.museum.iiif_key, iiif_key_safe, sizeof(order.museum.iiif_key));

    pinned_entry_file_t file = {0};
    file.magic = PINNED_ENTRY_MAGIC;
    file.version = PINNED_FORMAT_VERSION;
    file.post_id = original_post_id;
    file.source = PINNED_SOURCE_INSTITUTION;
    file.extension = extension;
    file.museum_id = museum_id;
    file.pinned_at = order.pinned_at;
    file.original_post_id = original_post_id;
    file.original_created_at = original_created_at;
    build_institution_source_id(museum_id, iiif_key_safe,
                                file.source_id, sizeof(file.source_id));
    if (title)   strlcpy(file.title, title,   sizeof(file.title));
    if (creator) strlcpy(file.creator, creator, sizeof(file.creator));

    return pin_list_pin(slug, &order, &file, src_artwork_path);
}

esp_err_t pin_lists_unpin_institution(const char *slug,
                                      uint16_t museum_id,
                                      const char *iiif_key_safe)
{
    if (!iiif_key_safe || !iiif_key_safe[0]) return ESP_ERR_INVALID_ARG;
    char source_id[PINNED_SOURCE_ID_MAX];
    build_institution_source_id(museum_id, iiif_key_safe, source_id, sizeof(source_id));
    return pin_list_unpin(slug, PINNED_SOURCE_INSTITUTION, source_id);
}
