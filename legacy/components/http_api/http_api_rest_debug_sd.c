// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file http_api_rest_debug_sd.c
 * @brief SD card raw-inspection endpoints (diagnostic builds only).
 *
 * Compiled only with CONFIG_P3A_SD_RAW_DEBUG. Lets a host inspect the card
 * underneath FatFS without a USB MSC session, so that a filesystem-level
 * fault (as opposed to a bus fault) can be analyzed offline:
 *
 *   GET /api/debug/sd/info                 card identity (CID/CSD/SCR/SSR,
 *                                          bus width, clock) + sd_health state
 *   GET /api/debug/sd/read?lba=N&count=M   raw sectors as octet-stream
 *                                          (M <= SD_RAW_MAX_SECTORS)
 *   GET /api/debug/sd/ls?path=/sdcard/...  what FatFS enumerates for a
 *                                          directory (name, type, size)
 *   GET /api/debug/sd/stat?path=...        stat() result or errno
 *   GET /api/debug/sd/unlink?path=...      unlink() one regular file under
 *                                          the SD root (repair clean-up)
 *
 * Read-only except for unlink, which refuses directories and anything
 * outside the configured SD root. Raw reads go through sdmmc_read_sectors(), which the
 * host driver serializes against FatFS traffic with its own mutex, so they
 * are safe while the volume is mounted (the view of not-yet-flushed FatFS
 * window data may lag by one sync).
 */

#include "sdkconfig.h"
#if CONFIG_P3A_SD_RAW_DEBUG

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"       // bsp_sdcard
#include "sdmmc_cmd.h"
#include "sd_health.h"
#include "sd_path.h"
#include "http_api_internal.h"

static const char *TAG = "dbg_sd";

#define SD_RAW_MAX_SECTORS    512     // per request (256 KB at 512 B sectors)
#define SD_RAW_CHUNK_SECTORS  32      // per sdmmc_read_sectors() call
#define SD_RAW_LS_MAX_ENTRIES 4000

static bool query_str(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char q[400];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, out_len) != ESP_OK) return false;
    url_decode_in_place(out);
    return true;
}

static bool query_u32(httpd_req_t *req, const char *key, uint32_t *out)
{
    char v[24];
    if (!query_str(req, key, v, sizeof(v))) return false;
    *out = (uint32_t)strtoul(v, NULL, 10);
    return true;
}

static esp_err_t h_sd_info(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        send_json_oom(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);

    sdmmc_card_t *card = bsp_sdcard;
    cJSON_AddBoolToObject(data, "mounted", card != NULL);
    cJSON_AddStringToObject(data, "sd_root", sd_path_get_root());
    sd_health_state_t hs = sd_health_get_state();
    cJSON_AddStringToObject(data, "sd_health",
                            hs == SD_HEALTH_OK ? "ok" : hs == SD_HEALTH_FAILED ? "failed" : "unknown");
    if (card) {
        char name[9] = {0};
        memcpy(name, card->cid.name, 8);
        cJSON *cid = cJSON_CreateObject();
        cJSON_AddNumberToObject(cid, "mfg_id", card->cid.mfg_id);
        cJSON_AddNumberToObject(cid, "oem_id", card->cid.oem_id);
        cJSON_AddStringToObject(cid, "name", name);
        cJSON_AddNumberToObject(cid, "revision", card->cid.revision);
        cJSON_AddNumberToObject(cid, "serial", (double)(uint32_t)card->cid.serial);
        cJSON_AddNumberToObject(cid, "date", card->cid.date);
        cJSON_AddItemToObject(data, "cid", cid);

        cJSON *csd = cJSON_CreateObject();
        cJSON_AddNumberToObject(csd, "csd_ver", card->csd.csd_ver);
        cJSON_AddNumberToObject(csd, "capacity_sectors", (double)(uint32_t)card->csd.capacity);
        cJSON_AddNumberToObject(csd, "sector_size", card->csd.sector_size);
        cJSON_AddNumberToObject(csd, "read_block_len", card->csd.read_block_len);
        cJSON_AddNumberToObject(csd, "card_command_class", card->csd.card_command_class);
        cJSON_AddNumberToObject(csd, "tr_speed", card->csd.tr_speed);
        cJSON_AddItemToObject(data, "csd", csd);

        cJSON *scr = cJSON_CreateObject();
        cJSON_AddNumberToObject(scr, "sd_spec", card->scr.sd_spec);
        cJSON_AddNumberToObject(scr, "bus_width", card->scr.bus_width);
        cJSON_AddNumberToObject(scr, "erase_mem_state", card->scr.erase_mem_state);
        cJSON_AddItemToObject(data, "scr", scr);

        cJSON *ssr = cJSON_CreateObject();
        cJSON_AddNumberToObject(ssr, "alloc_unit_kb", card->ssr.alloc_unit_kb);
        cJSON_AddNumberToObject(ssr, "erase_size_au", card->ssr.erase_size_au);
        cJSON_AddNumberToObject(ssr, "cur_bus_width", card->ssr.cur_bus_width);
        cJSON_AddNumberToObject(ssr, "discard_support", card->ssr.discard_support);
        cJSON_AddNumberToObject(ssr, "erase_timeout", card->ssr.erase_timeout);
        cJSON_AddNumberToObject(ssr, "erase_offset", card->ssr.erase_offset);
        cJSON_AddItemToObject(data, "ssr", ssr);

        cJSON_AddNumberToObject(data, "ocr", (double)card->ocr);
        cJSON_AddBoolToObject(data, "is_mmc", card->is_mmc);
        cJSON_AddBoolToObject(data, "is_ddr", card->is_ddr);
        cJSON_AddNumberToObject(data, "log_bus_width", card->log_bus_width);
        cJSON_AddNumberToObject(data, "card_max_freq_khz", (double)card->max_freq_khz);
        cJSON_AddNumberToObject(data, "real_freq_khz", card->real_freq_khz);
        cJSON_AddNumberToObject(data, "host_max_freq_khz", card->host.max_freq_khz);
        cJSON_AddNumberToObject(data, "host_slot", card->host.slot);
    }
    send_json_root(req, 200, root);
    return ESP_OK;
}

static esp_err_t h_sd_read(httpd_req_t *req)
{
    uint32_t lba = 0, count = 1;
    if (!query_u32(req, "lba", &lba)) {
        send_json_error(req, 400, "MISSING_LBA", "lba query parameter required");
        return ESP_OK;
    }
    query_u32(req, "count", &count);
    if (count == 0 || count > SD_RAW_MAX_SECTORS) {
        send_json_errorf(req, 400, "BAD_COUNT", "count must be 1..%d", SD_RAW_MAX_SECTORS);
        return ESP_OK;
    }
    sdmmc_card_t *card = bsp_sdcard;
    if (!card) {
        send_json_error(req, 503, "SD_NOT_MOUNTED", "SD card not initialized");
        return ESP_OK;
    }
    const uint32_t sector = (uint32_t)card->csd.sector_size;
    const uint32_t capacity = (uint32_t)card->csd.capacity;
    if (lba >= capacity || count > capacity - lba) {
        send_json_errorf(req, 400, "OUT_OF_RANGE", "lba+count exceeds capacity (%lu sectors)",
                         (unsigned long)capacity);
        return ESP_OK;
    }

    // Aligned PSRAM buffer: SDMMC DMA on the P4 needs 128 B (cache line)
    // alignment and whole-line sizes (jitter fix 1).
    const size_t chunk_bytes = (size_t)SD_RAW_CHUNK_SECTORS * sector;
    uint8_t *buf = heap_caps_aligned_alloc(128, chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        send_json_oom(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char lba_hdr[24];
    snprintf(lba_hdr, sizeof(lba_hdr), "%lu", (unsigned long)lba);
    httpd_resp_set_hdr(req, "X-SD-LBA", lba_hdr);
    char sec_hdr[16];
    snprintf(sec_hdr, sizeof(sec_hdr), "%lu", (unsigned long)sector);
    httpd_resp_set_hdr(req, "X-SD-Sector-Size", sec_hdr);

    esp_err_t err = ESP_OK;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done;
        if (n > SD_RAW_CHUNK_SECTORS) n = SD_RAW_CHUNK_SECTORS;
        err = sdmmc_read_sectors(card, buf, lba + done, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sdmmc_read_sectors(lba=%lu, n=%lu) failed: %s",
                     (unsigned long)(lba + done), (unsigned long)n, esp_err_to_name(err));
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, n * sector) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        done += n;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(buf);
    if (err != ESP_OK) {
        // Body already started; the client sees a short response. The log is the signal.
        ESP_LOGW(TAG, "raw read truncated at %lu/%lu sectors", (unsigned long)done, (unsigned long)count);
    }
    return ESP_OK;
}

static bool path_is_sd(const char *p)
{
    return strncmp(p, "/sdcard", 7) == 0;
}

static esp_err_t h_sd_ls(httpd_req_t *req)
{
    char path[300];
    if (!query_str(req, "path", path, sizeof(path)) || !path_is_sd(path)) {
        send_json_error(req, 400, "BAD_PATH", "path query parameter under /sdcard required");
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!root || !data || !entries) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(entries);
        send_json_oom(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "path", path);
    cJSON_AddItemToObject(data, "entries", entries);

    errno = 0;
    DIR *d = opendir(path);
    if (!d) {
        cJSON_AddNumberToObject(data, "opendir_errno", errno);
        send_json_root(req, 200, root);
        return ESP_OK;
    }
    int n = 0;
    bool truncated = false;
    char full[600];
    struct dirent *de;
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        if (n >= SD_RAW_LS_MAX_ENTRIES) {
            truncated = true;
            break;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        cJSON_AddStringToObject(e, "type", de->d_type == DT_DIR ? "dir" : "file");
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            cJSON_AddNumberToObject(e, "size", (double)st.st_size);
            cJSON_AddNumberToObject(e, "mtime", (double)st.st_mtime);
        } else {
            cJSON_AddNumberToObject(e, "stat_errno", errno);
        }
        cJSON_AddItemToArray(entries, e);
        n++;
        errno = 0;
    }
    cJSON_AddNumberToObject(data, "readdir_end_errno", errno);
    closedir(d);
    cJSON_AddNumberToObject(data, "count", n);
    cJSON_AddBoolToObject(data, "truncated", truncated);
    send_json_root(req, 200, root);
    return ESP_OK;
}

static esp_err_t h_sd_stat(httpd_req_t *req)
{
    char path[300];
    if (!query_str(req, "path", path, sizeof(path)) || !path_is_sd(path)) {
        send_json_error(req, 400, "BAD_PATH", "path query parameter under /sdcard required");
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        send_json_oom(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "path", path);
    struct stat st;
    errno = 0;
    if (stat(path, &st) == 0) {
        cJSON_AddBoolToObject(data, "exists", true);
        cJSON_AddStringToObject(data, "type", S_ISDIR(st.st_mode) ? "dir" : "file");
        cJSON_AddNumberToObject(data, "size", (double)st.st_size);
        cJSON_AddNumberToObject(data, "mtime", (double)st.st_mtime);
    } else {
        cJSON_AddBoolToObject(data, "exists", false);
        cJSON_AddNumberToObject(data, "errno", errno);
    }
    send_json_root(req, 200, root);
    return ESP_OK;
}

static esp_err_t h_sd_unlink(httpd_req_t *req)
{
    char path[300];
    if (!query_str(req, "path", path, sizeof(path)) || !path_is_sd(path)) {
        send_json_error(req, 400, "BAD_PATH", "path query parameter under /sdcard required");
        return ESP_OK;
    }
    const char *root = sd_path_get_root();
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0 || path[root_len] != '/') {
        send_json_error(req, 400, "OUTSIDE_ROOT", "path must be below the configured SD root");
        return ESP_OK;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        send_json_errorf(req, 404, "NOT_FOUND", "stat failed (errno=%d)", errno);
        return ESP_OK;
    }
    if (S_ISDIR(st.st_mode)) {
        send_json_error(req, 400, "IS_DIR", "refusing to unlink a directory");
        return ESP_OK;
    }
    cJSON *root_obj = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root_obj || !data) {
        cJSON_Delete(root_obj);
        cJSON_Delete(data);
        send_json_oom(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root_obj, "ok", true);
    cJSON_AddItemToObject(root_obj, "data", data);
    cJSON_AddStringToObject(data, "path", path);
    errno = 0;
    int rc = unlink(path);
    cJSON_AddBoolToObject(data, "unlinked", rc == 0);
    cJSON_AddNumberToObject(data, "errno", rc == 0 ? 0 : errno);
    send_json_root(req, 200, root_obj);
    return ESP_OK;
}

esp_err_t h_get_debug_sd_route(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strncmp(uri, "/api/debug/sd/info", 18) == 0) return h_sd_info(req);
    if (strncmp(uri, "/api/debug/sd/read", 18) == 0) return h_sd_read(req);
    if (strncmp(uri, "/api/debug/sd/ls", 16) == 0) return h_sd_ls(req);
    if (strncmp(uri, "/api/debug/sd/stat", 18) == 0) return h_sd_stat(req);
    if (strncmp(uri, "/api/debug/sd/unlink", 20) == 0) return h_sd_unlink(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}

#endif // CONFIG_P3A_SD_RAW_DEBUG
