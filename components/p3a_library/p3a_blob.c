// SPDX-License-Identifier: Apache-2.0
#include "p3a_blob.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs_atomic.h"
#include "mbedtls/sha256.h"
#include "p3a_err.h"
#include "sd_path.h"

static const char *TAG = "blob";

#define HASH_CHUNK_BYTES (32 * 1024)
#define SD_DMA_ALIGN     128  // address and size: the SD host then reads straight into PSRAM (docs/jitter, fix 1)

const char *p3a_blob_extension(p3a_format_t format)
{
    switch (format) {
        case P3A_FORMAT_WEBP: return ".webp";
        case P3A_FORMAT_GIF:  return ".gif";
        case P3A_FORMAT_PNG:  return ".png";
        case P3A_FORMAT_JPEG: return ".jpg";
        case P3A_FORMAT_BMP:  return ".bmp";
        default:              return NULL;
    }
}

static bool is_zero_ref(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN])
{
    for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
        if (ref[i] != 0) {
            return false;
        }
    }
    return true;
}

esp_err_t p3a_blob_path(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t format, char *out, size_t out_len)
{
    const char *ext = p3a_blob_extension(format);
    if (ref == NULL || out == NULL || ext == NULL || is_zero_ref(ref)) {
        return ESP_ERR_INVALID_ARG;
    }
    char name[P3A_RECORD_BLOB_REF_LEN * 2 + 1];
    for (int i = 0; i < P3A_RECORD_BLOB_REF_LEN; i++) {
        snprintf(name + i * 2, 3, "%02x", ref[i]);
    }
    char base[96];
    esp_err_t err = sd_path_get_subdir("blobs", base, sizeof(base));
    if (err != ESP_OK) {
        return err;
    }
    return sd_path_build_sharded(base, name, ext, out, out_len);
}

// The SHA-256 of a file. Reads with POSIX read() into an aligned buffer, as
// the player does and for the same reason (docs/jitter, fixes 1 and 6).
static esp_err_t hash_file(const char *path, uint8_t sha[32], size_t *size_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return (errno == ENOENT || errno == ENOTDIR) ? (esp_err_t)P3A_ERR_MISSING : ESP_FAIL;
    }
    // Aligned, so not through psram_alloc.h, which has no aligned form.
    uint8_t *chunk = heap_caps_aligned_alloc(SD_DMA_ALIGN, HASH_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t total = 0;
    esp_err_t err = ESP_OK;
    for (;;) {
        const ssize_t got = read(fd, chunk, HASH_CHUNK_BYTES);
        if (got < 0) {
            ESP_LOGW(TAG, "read error at %u of %s (errno %d)", (unsigned)total, path, errno);
            err = ESP_FAIL;
            break;
        }
        if (got == 0) {
            break;
        }
        mbedtls_sha256_update(&ctx, chunk, (size_t)got);
        total += (size_t)got;
        taskYIELD();  // megabytes of hashing must not starve the others
    }
    if (err == ESP_OK) {
        mbedtls_sha256_finish(&ctx, sha);
    }
    mbedtls_sha256_free(&ctx);
    free(chunk);
    close(fd);
    *size_out = total;
    return err;
}

static esp_err_t place(const char *path, p3a_format_t format, const uint8_t sha[32],
                       uint8_t ref[P3A_RECORD_BLOB_REF_LEN], bool *already);

esp_err_t p3a_blob_adopt(const char *path, p3a_format_t format, uint8_t ref[P3A_RECORD_BLOB_REF_LEN], bool *already)
{
    if (already) {
        *already = false;
    }
    if (path == NULL || ref == NULL || p3a_blob_extension(format) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t sha[32];
    size_t size = 0;
    esp_err_t err = hash_file(path, sha, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size == 0) {
        return P3A_ERR_CORRUPT;  // a download that never arrived
    }
    return place(path, format, sha, ref, already);
}

esp_err_t p3a_blob_adopt_hashed(const char *path, p3a_format_t format, const uint8_t sha256[32],
                                uint8_t ref[P3A_RECORD_BLOB_REF_LEN], bool *already)
{
    if (already) {
        *already = false;
    }
    if (path == NULL || ref == NULL || sha256 == NULL || p3a_blob_extension(format) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return P3A_ERR_MISSING;
    }
    if (st.st_size == 0) {
        return P3A_ERR_CORRUPT;
    }
    return place(path, format, sha256, ref, already);
}

// Names the file after its hash and moves it into the store.
static esp_err_t place(const char *path, p3a_format_t format, const uint8_t sha[32],
                       uint8_t ref[P3A_RECORD_BLOB_REF_LEN], bool *already)
{
    esp_err_t err;
    memcpy(ref, sha, P3A_RECORD_BLOB_REF_LEN);
    if (is_zero_ref(ref)) {
        ref[P3A_RECORD_BLOB_REF_LEN - 1] = 1;  // all zero means "no blob"; one chance in 2^128
    }
    char final[P3A_BLOB_PATH_MAX];
    err = p3a_blob_path(ref, format, final, sizeof(final));
    if (err != ESP_OK) {
        return err;
    }
    struct stat st;
    if (stat(final, &st) == 0) {
        // The store has these bytes. The new copy is not needed.
        if (already) {
            *already = true;
        }
        unlink(path);
        return ESP_OK;
    }
    err = sd_path_ensure_parent_dirs(final);
    if (err != ESP_OK) {
        return err;
    }
    return fs_atomic_rename(path, final, NULL);
}

bool p3a_blob_exists(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t format)
{
    char path[P3A_BLOB_PATH_MAX];
    struct stat st;
    return p3a_blob_path(ref, format, path, sizeof(path)) == ESP_OK && stat(path, &st) == 0;
}

esp_err_t p3a_blob_remove(const uint8_t ref[P3A_RECORD_BLOB_REF_LEN], p3a_format_t format)
{
    char path[P3A_BLOB_PATH_MAX];
    esp_err_t err = p3a_blob_path(ref, format, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    if (unlink(path) == 0) {
        return ESP_OK;
    }
    return errno == ENOENT ? (esp_err_t)P3A_ERR_MISSING : ESP_FAIL;
}
