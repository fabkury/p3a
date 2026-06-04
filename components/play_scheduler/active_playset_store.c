// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file active_playset_store.c
 * @brief Active-playset snapshot persistence (boot-restore storage).
 *
 * Stores the playset as: 64-byte header followed by channel_count copies of
 * ps_channel_spec_t verbatim. The struct layout is treated as opaque blob; if
 * the layout ever changes, bump ACTIVE_PLAYSET_VERSION and existing snapshots
 * are discarded on load (boot falls back to Makapix Promoted — acceptable for
 * a single-device snapshot that's rewritten on every channel switch).
 */

#include "active_playset_store.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "act_ps_store";

#define ACTIVE_PLAYSET_FILENAME      "active_playset.bin"
#define ACTIVE_PLAYSET_TMP_FILENAME  "active_playset.bin.tmp"
#define ACTIVE_PLAYSET_MAGIC     0x50415033u   /* 'P3AP' little-endian */
#define ACTIVE_PLAYSET_VERSION   1u

/* Room for the configured root plus "/active_playset.bin.tmp" */
#define ACTIVE_PLAYSET_PATH_MAX  (SD_PATH_ROOT_MAX_LEN + 32)

/**
 * Resolve {root}/{filename} against the configured SD root at runtime.
 * The snapshot deliberately lives under the user-configurable root so a
 * root switch behaves as a cold start (no cross-root playset bleed-through).
 */
static esp_err_t build_snapshot_path(const char *filename, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s/%s", sd_path_get_root(), filename);
    if (n < 0 || (size_t)n >= out_len) {
        ESP_LOGE(TAG, "Snapshot path truncated for %s", filename);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/*
 * Header. Channels are appended as channel_count copies of ps_channel_spec_t
 * (in-memory layout). Endianness: little-endian (ESP32). Cross-compiler
 * portability is not a goal — if sizeof(ps_channel_spec_t) ever differs from
 * the writer's, the version bump or CRC failure makes the load discard.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t channel_count;          /* [1, PS_MAX_CHANNELS] */
    uint32_t spec_size;              /* sizeof(ps_channel_spec_t) at write time */
    uint32_t checksum;               /* CRC32 of header(checksum=0) + channels */
    char     name[PS_PLAYSET_NAME_MAX + 1];   /* 33 bytes; may be empty */
    uint8_t  reserved[15];           /* pad to 64 */
} active_playset_header_t;

_Static_assert(sizeof(active_playset_header_t) == 64,
               "active-playset header must be 64 bytes");

static uint32_t compute_checksum(const active_playset_header_t *header,
                                 const ps_channel_spec_t *channels,
                                 size_t channel_count)
{
    active_playset_header_t hdr_zero = *header;
    hdr_zero.checksum = 0;
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&hdr_zero, sizeof(hdr_zero));
    if (channels && channel_count > 0) {
        crc = esp_rom_crc32_le(crc, (const uint8_t *)channels,
                               channel_count * sizeof(ps_channel_spec_t));
    }
    return crc;
}

esp_err_t active_playset_save(const ps_playset_t *playset)
{
    if (!playset) return ESP_ERR_INVALID_ARG;
    if (playset->channel_count == 0 || playset->channel_count > PS_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel_count: %zu", playset->channel_count);
        return ESP_ERR_INVALID_ARG;
    }

    char path[ACTIVE_PLAYSET_PATH_MAX];
    char tmp_path[ACTIVE_PLAYSET_PATH_MAX];
    esp_err_t path_err = build_snapshot_path(ACTIVE_PLAYSET_FILENAME, path, sizeof(path));
    if (path_err == ESP_OK) {
        path_err = build_snapshot_path(ACTIVE_PLAYSET_TMP_FILENAME, tmp_path, sizeof(tmp_path));
    }
    if (path_err != ESP_OK) {
        return path_err;
    }

    active_playset_header_t header = {0};
    header.magic = ACTIVE_PLAYSET_MAGIC;
    header.version = ACTIVE_PLAYSET_VERSION;
    header.channel_count = (uint16_t)playset->channel_count;
    header.spec_size = (uint32_t)sizeof(ps_channel_spec_t);
    strlcpy(header.name, playset->name, sizeof(header.name));
    header.checksum = compute_checksum(&header, playset->channels, playset->channel_count);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", tmp_path, strerror(errno));
        return ESP_FAIL;
    }

    bool write_ok = true;
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGE(TAG, "write header failed");
        write_ok = false;
    }
    if (write_ok &&
        fwrite(playset->channels, sizeof(ps_channel_spec_t),
               playset->channel_count, f) != playset->channel_count) {
        ESP_LOGE(TAG, "write channels failed");
        write_ok = false;
    }
    if (write_ok) {
        fflush(f);
        fsync(fileno(f));
    }
    fclose(f);

    if (!write_ok) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    /* Atomic replace: unlink target then rename. On POSIX, rename(2) is
       atomic over the same filesystem; the unlink is belt-and-braces for
       filesystems where rename refuses to overwrite. */
    unlink(path);
    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s",
                 tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved active playset (name='%s', channels=%zu)",
             playset->name, playset->channel_count);
    return ESP_OK;
}

esp_err_t active_playset_load(ps_playset_t *out_playset)
{
    if (!out_playset) return ESP_ERR_INVALID_ARG;

    char path[ACTIVE_PLAYSET_PATH_MAX];
    esp_err_t path_err = build_snapshot_path(ACTIVE_PLAYSET_FILENAME, path, sizeof(path));
    if (path_err != ESP_OK) {
        return path_err;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    active_playset_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGW(TAG, "Truncated header — discarding");
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    if (header.magic != ACTIVE_PLAYSET_MAGIC) {
        ESP_LOGW(TAG, "Bad magic 0x%08lx — discarding",
                 (unsigned long)header.magic);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    if (header.version != ACTIVE_PLAYSET_VERSION) {
        ESP_LOGW(TAG, "Version mismatch (%u, expected %u) — discarding",
                 header.version, ACTIVE_PLAYSET_VERSION);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Layout sanity: writer and reader must agree on ps_channel_spec_t size.
       Treated like a version mismatch — discard and fall back. */
    if (header.spec_size != sizeof(ps_channel_spec_t)) {
        ESP_LOGW(TAG, "Spec size mismatch (file=%lu, expected=%zu) — discarding",
                 (unsigned long)header.spec_size, sizeof(ps_channel_spec_t));
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_VERSION;
    }

    if (header.channel_count == 0 || header.channel_count > PS_MAX_CHANNELS) {
        ESP_LOGW(TAG, "Bad channel_count %u — discarding", header.channel_count);
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    memset(out_playset, 0, sizeof(*out_playset));
    header.name[sizeof(header.name) - 1] = '\0';
    strlcpy(out_playset->name, header.name, sizeof(out_playset->name));
    out_playset->channel_count = header.channel_count;

    if (fread(out_playset->channels, sizeof(ps_channel_spec_t),
              header.channel_count, f) != header.channel_count) {
        ESP_LOGW(TAG, "Truncated channels — discarding");
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }
    fclose(f);

    uint32_t expected = header.checksum;
    uint32_t actual = compute_checksum(&header, out_playset->channels,
                                       out_playset->channel_count);
    if (actual != expected) {
        ESP_LOGW(TAG, "CRC mismatch (file=0x%08lx, computed=0x%08lx) — discarding",
                 (unsigned long)expected, (unsigned long)actual);
        unlink(path);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "Loaded active playset (name='%s', channels=%zu)",
             out_playset->name, out_playset->channel_count);
    return ESP_OK;
}

esp_err_t active_playset_clear(void)
{
    char path[ACTIVE_PLAYSET_PATH_MAX];
    esp_err_t path_err = build_snapshot_path(ACTIVE_PLAYSET_FILENAME, path, sizeof(path));
    if (path_err != ESP_OK) {
        return path_err;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "unlink %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}
