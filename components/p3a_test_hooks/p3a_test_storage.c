// SPDX-License-Identifier: Apache-2.0
// Test console commands for the storage layer.

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#include "fs_atomic.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_frame.h"
#include "p3a_storage.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"
#include "sd_health.h"
#include "sd_path.h"
#include "sd_repair.h"

static atomic_int s_health_events;
static bool s_subscribed;

static void on_health_failed(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    atomic_fetch_add(&s_health_events, 1);
}

static void subscribe_once(void)
{
    if (!s_subscribed) {
        s_subscribed = p3a_event_subscribe(P3A_EVENT_SD_HEALTH_FAILED, on_health_failed, NULL) == ESP_OK;
    }
}

static const char *health_name(void)
{
    switch (sd_health_get_state()) {
    case SD_HEALTH_OK:
        return "ok";
    case SD_HEALTH_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void scratch_path(char *out, size_t out_len, const char *name)
{
    char dir[96];
    sd_path_get_temporary(dir, sizeof(dir));
    snprintf(out, out_len, "%s/%s", dir, name);
}

static void cmd_info(void)
{
    const sd_repair_fat_stats_t *fat = sd_repair_fat_stats();
    const p3a_storage_space_t space = p3a_storage_space();
    char json[512];
    snprintf(json, sizeof(json),
             "{\"root\":\"%s\",\"health\":\"%s\",\"health_events\":%d,\"repairs_boot\":%u,\"repairs_total\":%u,"
             "\"fat_check_ran\":%s,\"fat_sectors\":%u,\"fat_mismatched\":%u,\"fat_restored\":%u,\"fat_check_ms\":%u,"
             "\"space_valid\":%s,\"total_mb\":%u,\"free_mb\":%u,\"space_age_ms\":%d}",
             sd_path_get_root(), health_name(), atomic_load(&s_health_events), (unsigned)sd_repair_count_boot(),
             (unsigned)sd_repair_count_total(), fat->ran ? "true" : "false", (unsigned)fat->fat_sectors,
             (unsigned)fat->mismatched, (unsigned)(fat->repaired_fat1 + fat->repaired_fat2), (unsigned)fat->elapsed_ms,
             space.valid ? "true" : "false", (unsigned)(space.total_bytes >> 20), (unsigned)(space.free_bytes >> 20),
             space.valid ? (int)((esp_timer_get_time() - space.measured_at_us) / 1000) : -1);
    p3a_test_reply_ok(json);
}

// Writes `bytes` of a pattern atomically, twice (the second replaces the
// first), reads the result back, and checks that no temporary file is left.
static void cmd_atomic(const char *name, int bytes)
{
    char path[160], tmp[168];
    scratch_path(path, sizeof(path), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    uint8_t *out = psram_malloc((size_t)bytes);
    uint8_t *in = psram_malloc((size_t)bytes);
    const char *failure = NULL;
    esp_err_t err = ESP_OK;
    if (!out || !in) {
        failure = "no memory";
    } else {
        memset(out, 0x11, (size_t)bytes);
        err = fs_atomic_write(path, out, (size_t)bytes, NULL);
        if (err == ESP_OK) {
            for (int i = 0; i < bytes; i++) {
                out[i] = (uint8_t)(i * 7 + 3);
            }
            err = fs_atomic_write(path, out, (size_t)bytes, NULL);  // replaces an existing file
        }
        if (err != ESP_OK) {
            failure = p3a_err_to_name(err);
        } else {
            FILE *f = fopen(path, "rb");
            if (!f || fread(in, 1, (size_t)bytes, f) != (size_t)bytes || memcmp(in, out, (size_t)bytes) != 0) {
                failure = "read back different bytes";
            }
            if (f) {
                fclose(f);
            }
        }
    }
    const bool tmp_left = exists(tmp);
    unlink(path);
    free(out);
    free(in);
    if (failure) {
        p3a_test_reply_err(failure);
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"bytes\":%d,\"tmp_left\":%s}", bytes, tmp_left ? "true" : "false");
    p3a_test_reply_ok(json);
}

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint8_t body[12];
} test_record_t;

// Saves `count` records as a container, loads them back, then damages the
// file in three ways and reports what the loader says each time. The file
// must still exist after every failed load.
static void cmd_frame(int count)
{
    const p3a_frame_expect_t expect = {
        .schema = P3A_FRAME_SCHEMA_TEST, .version = 1, .record_size = sizeof(test_record_t), .max_count = 100000};
    char path[160];
    scratch_path(path, sizeof(path), "frame-test.p3af");
    unlink(path);

    void *loaded = NULL;
    uint32_t n = 0, flags = 0;
    const char *missing = p3a_err_to_name(p3a_frame_load(path, &expect, &loaded, &n, &flags));

    test_record_t *records = psram_calloc((size_t)(count ? count : 1), sizeof(test_record_t));
    if (!records) {
        p3a_test_reply_err("no memory");
        return;
    }
    for (int i = 0; i < count; i++) {
        records[i].id = (uint32_t)i * 2654435761u;
        memset(records[i].body, i & 0xFF, sizeof(records[i].body));
    }
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = p3a_frame_save(path, P3A_FRAME_SCHEMA_TEST, 1, sizeof(test_record_t), (uint32_t)count,
                                   P3A_FRAME_REBUILDABLE, records);
    const int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        free(records);
        p3a_test_reply_err(p3a_err_to_name(err));
        return;
    }
    err = p3a_frame_load(path, &expect, &loaded, &n, &flags);
    const int64_t t2 = esp_timer_get_time();
    const bool same = err == ESP_OK && n == (uint32_t)count &&
                      (count == 0 || memcmp(loaded, records, (size_t)count * sizeof(test_record_t)) == 0);
    free(loaded);
    free(records);

    // Another firmware's file: intact, but a version this caller does not expect.
    p3a_frame_expect_t other = expect;
    other.version = 2;
    const char *version = p3a_err_to_name(p3a_frame_load(path, &other, &loaded, &n, NULL));

    // Damage: flip one byte in the middle of the records (or of the header when there are none).
    const char *flipped = "skipped";
    const char *truncated = "skipped";
    FILE *f = fopen(path, "r+b");
    if (f) {
        const long at = count > 0 ? 32 + (long)count * (long)sizeof(test_record_t) / 2 : 10;
        uint8_t byte = 0;
        fseek(f, at, SEEK_SET);
        fread(&byte, 1, 1, f);
        byte ^= 0x40;
        fseek(f, at, SEEK_SET);
        fwrite(&byte, 1, 1, f);
        fclose(f);
        flipped = p3a_err_to_name(p3a_frame_load(path, &expect, &loaded, &n, NULL));
    }
    const bool kept_after_flip = exists(path);

    // Damage: cut the file short.
    if (count > 0 && truncate(path, 32 + (off_t)sizeof(test_record_t) / 2) == 0) {
        truncated = p3a_err_to_name(p3a_frame_load(path, &expect, &loaded, &n, NULL));
    }
    const bool kept_after_truncate = exists(path);
    unlink(path);

    char json[400];
    snprintf(json, sizeof(json),
             "{\"count\":%d,\"round_trip\":%s,\"flags\":%u,\"save_ms\":%d,\"load_ms\":%d,\"missing\":\"%s\","
             "\"other_version\":\"%s\",\"flipped\":\"%s\",\"truncated\":\"%s\",\"kept_after_flip\":%s,"
             "\"kept_after_truncate\":%s}",
             count, same ? "true" : "false", (unsigned)flags, (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000), missing,
             version, flipped, truncated, kept_after_flip ? "true" : "false", kept_after_truncate ? "true" : "false");
    p3a_test_reply_ok(json);
}

// !storage info
// !storage space                   measures free space now and reports how long it took
// !storage atomic <name> <bytes>   atomic write, replace and read-back in the temporary folder
// !storage frame <count>           container round trip and three kinds of damage
// !storage shard <leaf> <ext>      where the shard layout puts a name
// !storage parents <relative>      creates the folders on the way to {root}/<relative>
// !storage latch                   reports three failed writes: the card is failed until reboot
static void cmd_storage(const char *args)
{
    char verb[12] = {0}, a[96] = {0}, b[16] = {0};
    const int n = sscanf(args, "%11s %95s %15s", verb, a, b);
    subscribe_once();

    if (n >= 1 && strcmp(verb, "info") == 0) {
        cmd_info();
    } else if (n >= 1 && strcmp(verb, "space") == 0) {
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = p3a_storage_space_refresh();
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
            return;
        }
        const p3a_storage_space_t space = p3a_storage_space();
        char json[128];
        snprintf(json, sizeof(json), "{\"total_mb\":%u,\"free_mb\":%u,\"took_ms\":%d}", (unsigned)(space.total_bytes >> 20),
                 (unsigned)(space.free_bytes >> 20), (int)((esp_timer_get_time() - t0) / 1000));
        p3a_test_reply_ok(json);
    } else if (n == 3 && strcmp(verb, "atomic") == 0 && atoi(b) > 0 && atoi(b) <= 2 * 1024 * 1024) {
        cmd_atomic(a, atoi(b));
    } else if (n == 2 && strcmp(verb, "frame") == 0 && atoi(a) >= 0 && atoi(a) <= 100000) {
        cmd_frame(atoi(a));
    } else if (n >= 2 && strcmp(verb, "shard") == 0) {
        char base[96], path[256], json[300];
        sd_path_get_subdir("shard-test", base, sizeof(base));
        esp_err_t err = sd_path_build_sharded(base, a, n == 3 ? b : "", path, sizeof(path));
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
            return;
        }
        snprintf(json, sizeof(json), "{\"path\":\"%s\"}", path);
        p3a_test_reply_ok(json);
    } else if (n == 2 && strcmp(verb, "parents") == 0) {
        char path[200], json[64];
        snprintf(path, sizeof(path), "%s/%s", sd_path_get_root(), a);
        esp_err_t err = sd_path_ensure_parent_dirs(path);
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
            return;
        }
        char *slash = strrchr(path, '/');
        *slash = '\0';
        struct stat st;
        const bool made = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        // Tidy up: remove the folders just created, deepest first, down to the root.
        const size_t root_len = strlen(sd_path_get_root());
        while (strlen(path) > root_len && rmdir(path) == 0) {
            slash = strrchr(path, '/');
            *slash = '\0';
        }
        snprintf(json, sizeof(json), "{\"created\":%s}", made ? "true" : "false");
        p3a_test_reply_ok(json);
    } else if (n >= 1 && strcmp(verb, "latch") == 0) {
        char path[160];
        scratch_path(path, sizeof(path), "pretend-failure");
        sd_health_report_write_failure(path, ENOSPC);  // a full card is not a failed card
        const bool after_full = sd_health_is_failed();
        for (int i = 0; i < SD_HEALTH_FAIL_THRESHOLD; i++) {
            sd_health_report_write_failure(path, EIO);
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // let the event be delivered
        const char *write_after = p3a_err_to_name(fs_atomic_write(path, "x", 1, NULL));
        char json[200];
        snprintf(json, sizeof(json),
                 "{\"failed_after_disk_full\":%s,\"failed\":%s,\"health\":\"%s\",\"events\":%d,\"write_after\":\"%s\"}",
                 after_full ? "true" : "false", sd_health_is_failed() ? "true" : "false", health_name(),
                 atomic_load(&s_health_events), write_after);
        p3a_test_reply_ok(json);
    } else {
        p3a_test_reply_err("usage: !storage <info|space|atomic name bytes|frame count|shard leaf [ext]|parents rel|latch>");
    }
}

const p3a_test_command_t p3a_test_storage_commands[] = {
    {"storage", cmd_storage},
};
const size_t p3a_test_storage_command_count = sizeof(p3a_test_storage_commands) / sizeof(p3a_test_storage_commands[0]);
