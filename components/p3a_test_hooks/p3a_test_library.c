// SPDX-License-Identifier: Apache-2.0
// Test commands for the library: indices on the card and the blob store.
//
// !library index <count>               saves an index of <count> records, loads it back, then
//                                      damages the file in three ways and reports each result
// !library blob <bytes> <seed> <fmt>   makes a file of <bytes> bytes from <seed>, takes it into
//                                      the blob store as format number <fmt>
// !library blob-remove <ref> <fmt>     removes the blob with this 32-digit reference
// !library blob-has <ref> <fmt>        whether the store holds it
// !library evict <bytes> <min_age_s> <keep ref>   deletes the oldest blobs until <bytes> are
//                                      freed, sparing the younger ones and the one named
// !library local-put <name> <bytes>    makes a file of that size in the user's folder
// !library local-remove <name>
// !library local-scan <capacity>       lists the folder through the local source's table

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#include "p3a_blob.h"
#include "p3a_err.h"
#include "p3a_evict.h"
#include "p3a_frame.h"
#include "p3a_index.h"
#include "p3a_source.h"
#include "source_local.h"
#include "p3a_test_internal.h"
#include "psram_alloc.h"
#include "sd_path.h"

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void scratch_path(char *out, size_t len, const char *name)
{
    char dir[128];
    sd_path_get_temporary(dir, sizeof(dir));
    snprintf(out, len, "%s/%s", dir, name);
}

// Record n of the test index: the sources in turn, every third with a blob,
// every eleventh flagged gone.
static esp_err_t make_record(p3a_record_t *r, uint32_t n)
{
    char text[64];
    uint8_t uuid[16];
    esp_err_t err;
    switch (n % 5) {
        case 0:
            snprintf(text, sizeof(text), "local file %05u.gif", (unsigned)n);
            err = p3a_record_init_local(r, text);
            break;
        case 1:
            memset(uuid, (int)(n & 0xFF), sizeof(uuid));
            memcpy(uuid, &n, sizeof(n));
            err = p3a_record_init_makapix(r, n + 1000, uuid);
            break;
        case 2:
            snprintf(text, sizeof(text), "giphy%07u", (unsigned)n);
            err = p3a_record_init_giphy(r, text);
            break;
        case 3:
            err = p3a_record_init_klipy(r, 9000000000ull + n, (uint8_t)(n & 1));
            break;
        default:
            snprintf(text, sizeof(text), "iiif/3/key-%06u/full/max/0/default.jpg", (unsigned)n);
            err = p3a_record_init_museum(r, (uint16_t)(n % 7), text);
            break;
    }
    if (err != ESP_OK) {
        return err;
    }
    r->sort_at = 1700000000u + n;
    r->format = (uint8_t)(1 + n % 5);
    if (n % 3 == 0) {
        memset(r->blob_ref, 0xB0, sizeof(r->blob_ref));
        memcpy(r->blob_ref, &n, sizeof(n));
    }
    if (n % 11 == 0) {
        r->flags |= P3A_RECORD_GONE;
    }
    return ESP_OK;
}

static void cmd_index(int count)
{
    if (count < 2 || count > P3A_INDEX_MAX_RECORDS) {
        p3a_test_reply_err("count must be 2 to 4096");
        return;
    }
    char path[192];
    scratch_path(path, sizeof(path), "index-test.p3af");
    unlink(path);
    p3a_index_t made = {0}, back = {0}, small = {0};
    if (p3a_index_create(&made, (uint32_t)count) != ESP_OK || p3a_index_create(&back, (uint32_t)count) != ESP_OK ||
        p3a_index_create(&small, (uint32_t)count - 1) != ESP_OK) {
        p3a_index_destroy(&made);
        p3a_index_destroy(&back);
        p3a_index_destroy(&small);
        p3a_test_reply_err("no memory");
        return;
    }
    const char *missing = p3a_err_to_name(p3a_index_load(&back, path));
    for (uint32_t n = 0; n < (uint32_t)count; n++) {
        p3a_record_t r;
        if (make_record(&r, n) != ESP_OK || p3a_index_append(&made, &r, NULL) != ESP_OK) {
            p3a_test_reply_err("a test record was refused");
            goto done;
        }
    }
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = p3a_index_save(&made, path, P3A_FRAME_REBUILDABLE);
    const int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        p3a_test_reply_err(p3a_err_to_name(err));
        goto done;
    }
    struct stat st = {0};
    stat(path, &st);
    err = p3a_index_load(&back, path);
    const int64_t t2 = esp_timer_get_time();
    bool same = err == ESP_OK && back.count == made.count && back.available_count == made.available_count &&
                memcmp(back.records, made.records, (size_t)made.count * sizeof(p3a_record_t)) == 0;
    // Every record is found again by its identity, at its own position.
    for (uint32_t n = 0; same && n < made.count; n++) {
        same = p3a_index_find_record(&back, &made.records[n]) == (int32_t)n &&
               p3a_index_is_available(&back, n) == p3a_index_is_available(&made, n);
    }
    const uint32_t available = back.available_count;

    // A file with more records than the caller has room for.
    const char *too_many = p3a_err_to_name(p3a_index_load(&small, path));

    // Damage: one byte flipped inside the records.
    const char *flipped = "skipped";
    FILE *f = fopen(path, "r+b");
    if (f) {
        const long at = 32 + (long)(count / 2) * (long)sizeof(p3a_record_t) + 70;
        uint8_t byte = 0;
        fseek(f, at, SEEK_SET);
        fread(&byte, 1, 1, f);
        byte ^= 0x01;
        fseek(f, at, SEEK_SET);
        fwrite(&byte, 1, 1, f);
        fclose(f);
        flipped = p3a_err_to_name(p3a_index_load(&back, path));
    }
    const bool kept_after_flip = exists(path);
    const uint32_t count_after_flip = back.count;

    // A container that is intact (its checksums hold) but carries a record
    // no firmware should have written: the last one has no terminator.
    memset(made.records[count - 1].identity.raw, 'x', sizeof(made.records[count - 1].identity.raw));
    const char *bad_record = "skipped";
    if (p3a_frame_save(path, P3A_FRAME_SCHEMA_INDEX, P3A_RECORD_VERSION, sizeof(p3a_record_t), made.count,
                       P3A_FRAME_REBUILDABLE, made.records) == ESP_OK) {
        bad_record = p3a_err_to_name(p3a_index_load(&back, path));
    }
    const uint32_t count_after_bad = back.count;
    const bool kept_after_bad = exists(path);
    unlink(path);

    char json[520];
    snprintf(json, sizeof(json),
             "{\"count\":%d,\"round_trip\":%s,\"available\":%u,\"file_bytes\":%u,\"save_ms\":%d,\"load_ms\":%d,"
             "\"missing\":\"%s\",\"too_many\":\"%s\",\"flipped\":\"%s\",\"kept_after_flip\":%s,"
             "\"count_after_flip\":%u,\"bad_record\":\"%s\",\"kept_after_bad\":%s,\"count_after_bad\":%u}",
             count, same ? "true" : "false", (unsigned)available, (unsigned)st.st_size, (int)((t1 - t0) / 1000),
             (int)((t2 - t1) / 1000), missing, too_many, flipped, kept_after_flip ? "true" : "false",
             (unsigned)count_after_flip, bad_record, kept_after_bad ? "true" : "false", (unsigned)count_after_bad);
    p3a_test_reply_ok(json);
done:
    p3a_index_destroy(&made);
    p3a_index_destroy(&back);
    p3a_index_destroy(&small);
}

static bool parse_ref(const char *hex, uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
        unsigned byte = 0;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            return false;
        }
        ref[i] = (uint8_t)byte;
    }
    return true;
}

// Byte i of the test file for a seed. The device tests make the same bytes
// to know what the blob's name must be.
static uint8_t test_byte(unsigned seed, size_t i)
{
    return (uint8_t)(seed * 31u + i * 7u + (i >> 8));
}

static void cmd_blob(unsigned bytes, unsigned seed, int format)
{
    if (bytes > 4u * 1024 * 1024) {
        p3a_test_reply_err("at most 4 MiB");
        return;
    }
    char tmp[192];
    scratch_path(tmp, sizeof(tmp), "blob-test.tmp");
    uint8_t *content = psram_malloc(bytes ? bytes : 1);
    FILE *f = content ? fopen(tmp, "wb") : NULL;
    if (f == NULL) {
        free(content);
        p3a_test_reply_err("could not make the test file");
        return;
    }
    for (size_t i = 0; i < bytes; i++) {
        content[i] = test_byte(seed, i);
    }
    fwrite(content, 1, bytes, f);
    fclose(f);
    free(content);

    uint8_t ref[P3A_RECORD_BLOB_REF_LEN] = {0};
    bool already = false;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = p3a_blob_adopt(tmp, (p3a_format_t)format, ref, &already);
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    char path[P3A_BLOB_PATH_MAX] = "";
    struct stat st = {0};
    if (err == ESP_OK) {
        p3a_blob_path(ref, (p3a_format_t)format, path, sizeof(path));
        stat(path, &st);
    }
    char hex[P3A_RECORD_BLOB_REF_LEN * 2 + 1] = "";
    for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
        snprintf(hex + i * 2, 3, "%02x", ref[i]);
    }
    char json[480];
    snprintf(json, sizeof(json),
             "{\"result\":\"%s\",\"ref\":\"%s\",\"path\":\"%s\",\"already\":%s,\"exists\":%s,\"stored_bytes\":%u,"
             "\"upload_left\":%s,\"ms\":%d}",
             err == ESP_OK ? "ok" : p3a_err_to_name(err), hex, path, already ? "true" : "false",
             (err == ESP_OK && p3a_blob_exists(ref, (p3a_format_t)format)) ? "true" : "false", (unsigned)st.st_size,
             exists(tmp) ? "true" : "false", ms);
    unlink(tmp);  // only present when the adoption failed
    p3a_test_reply_ok(json);
}

static void cmd_blob_remove(const char *hex, int format)
{
    uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
    if (strlen(hex) < P3A_RECORD_BLOB_REF_LEN * 2 || !parse_ref(hex, ref)) {
        p3a_test_reply_err("give the 32-digit reference");
        return;
    }
    const esp_err_t err = p3a_blob_remove(ref, (p3a_format_t)format);
    char json[120];
    snprintf(json, sizeof(json), "{\"result\":\"%s\",\"exists\":%s}", err == ESP_OK ? "ok" : p3a_err_to_name(err),
             p3a_blob_exists(ref, (p3a_format_t)format) ? "true" : "false");
    p3a_test_reply_ok(json);
}

static void local_file(const char *name, char *out, size_t len)
{
    char dir[160];
    sd_path_get_animations(dir, sizeof(dir));
    snprintf(out, len, "%s/%s", dir, name);
}

static void cmd_local_put(const char *name, unsigned bytes)
{
    char path[440];
    local_file(name, path, sizeof(path));
    sd_path_ensure_parent_dirs(path);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        p3a_test_reply_err("could not make the file");
        return;
    }
    for (unsigned i = 0; i < bytes; i++) {
        fputc((int)(i & 0xFF), f);
    }
    fclose(f);
    p3a_test_reply_ok("{}");
}

// Lists the folder as the scheduler will: through the registered table.
static void cmd_local_scan(unsigned capacity)
{
    const p3a_source_ops_t *ops = p3a_source_get(P3A_SOURCE_LOCAL);
    p3a_index_t index = {0};
    if (ops == NULL || p3a_index_create(&index, capacity) != ESP_OK) {
        p3a_test_reply_err(ops ? "no memory, or a capacity out of range" : "the local source is not registered");
        return;
    }
    const p3a_channel_ref_t channel = {.kind = "", .identifier = "", .offset = 0};
    const esp_err_t err = ops->refresh(&channel, &index);
    source_local_scan_t scan;
    source_local_last_scan(&scan);
    char first_path[300] = "";
    if (index.count > 0) {
        ops->path(p3a_index_at(&index, 0), first_path, sizeof(first_path));
    }
    char json[900];
    int n = snprintf(json, sizeof(json),
                     "{\"result\":\"%s\",\"count\":%u,\"available\":%u,\"not_pictures\":%u,\"name_too_long\":%u,"
                     "\"over_capacity\":%u,\"ms\":%u,\"smooth\":%s,\"first_path\":\"%s\",\"first_exists\":%s,"
                     "\"names\":[",
                     err == ESP_OK ? "ok" : p3a_err_to_name(err), (unsigned)index.count,
                     (unsigned)index.available_count, (unsigned)scan.not_pictures, (unsigned)scan.name_too_long,
                     (unsigned)scan.over_capacity, (unsigned)scan.ms, ops->smooth ? "true" : "false", first_path,
                     (first_path[0] && exists(first_path)) ? "true" : "false");
    // The first eight names and formats, in the order the source gave them.
    for (uint32_t i = 0; i < index.count && i < 8 && n < (int)sizeof(json) - 160; i++) {
        const p3a_record_t *r = p3a_index_at(&index, i);
        n += snprintf(json + n, sizeof(json) - (size_t)n, "%s[\"%s\",%u]", i ? "," : "", r->identity.local.filename,
                      (unsigned)r->format);
    }
    snprintf(json + n, sizeof(json) - (size_t)n, "]}");
    p3a_index_destroy(&index);
    p3a_test_reply_ok(json);
}

static bool keep_one(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], void *ctx)
{
    return memcmp(ref, ctx, P3A_RECORD_BLOB_REF_LEN) == 0;
}

static void cmd_evict(uint64_t need, uint32_t min_age_s, const char *keep_hex)
{
    uint8_t keep[P3A_RECORD_BLOB_REF_LEN] = {0};
    parse_ref(keep_hex, keep);  // anything that is not a reference keeps nothing
    p3a_blob_evict_t done = {0};
    const esp_err_t err = p3a_blob_evict(need, min_age_s, keep_one, keep, &done);
    char text[160];
    snprintf(text, sizeof(text), "{\"result\":\"%s\",\"seen\":%u,\"deleted\":%u,\"freed\":%llu}",
             err == ESP_OK ? "ok" : p3a_err_to_name(err), (unsigned)done.seen, (unsigned)done.deleted,
             (unsigned long long)done.freed_bytes);
    p3a_test_reply_ok(text);
}

static void cmd_library(const char *args)
{
    unsigned a = 0, b = 0;
    int format = 0;
    char hex[40] = "";
    char name[200] = "";
    if (sscanf(args, " local-put %199s %u", name, &a) == 2) {
        cmd_local_put(name, a);
        return;
    }
    if (sscanf(args, " local-remove %199s", name) == 1) {
        char path[440];
        local_file(name, path, sizeof(path));
        p3a_test_reply_ok(unlink(path) == 0 ? "{\"removed\":true}" : "{\"removed\":false}");
        return;
    }
    if (sscanf(args, " local-scan %u", &a) == 1) {
        cmd_local_scan(a);
        return;
    }
    unsigned long long need = 0;
    if (sscanf(args, " index %u", &a) == 1) {
        cmd_index((int)a);
    } else if (sscanf(args, " evict %llu %u %39s", &need, &a, hex) == 3) {
        cmd_evict(need, a, hex);
    } else if (sscanf(args, " blob-has %39s %d", hex, &format) == 2) {
        uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
        if (!parse_ref(hex, ref)) {
            p3a_test_reply_err("a reference is 32 hex digits");
        } else {
            p3a_test_reply_ok(p3a_blob_exists(ref, (p3a_format_t)format) ? "{\"has\":true}" : "{\"has\":false}");
        }
    } else if (sscanf(args, " blob-remove %39s %d", hex, &format) == 2) {
        cmd_blob_remove(hex, format);
    } else if (sscanf(args, " blob %u %u %d", &a, &b, &format) == 3) {
        cmd_blob(a, b, format);
    } else {
        p3a_test_reply_err("library index <count> | blob <bytes> <seed> <format> | blob-remove <ref> <format>");
    }
}

const p3a_test_command_t p3a_test_library_commands[] = {
    {"library", cmd_library},
};
const size_t p3a_test_library_command_count = sizeof(p3a_test_library_commands) / sizeof(p3a_test_library_commands[0]);
