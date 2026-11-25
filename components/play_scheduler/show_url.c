// SPDX-License-Identifier: Apache-2.0
// "Show this": the job. See show_url.h.

#include "show_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "animation_player.h"
#include "esp_log.h"
#include "fs_atomic.h"
#include "http_fetch.h"
#include "p3a_err.h"
#include "p3a_limits.h"
#include "p3a_mem.h"
#include "p3a_storage.h"
#include "play_scheduler.h"
#include "psram_alloc.h"
#include "sd_health.h"
#include "sd_path.h"
#include "source_local.h"

static const char *TAG = "show_url";

static void on_progress(size_t total, size_t content_length, void *ctx)
{
    (void)ctx;
    if (content_length > 0) {
        const size_t percent = total * 100 / content_length;
        p3a_job_set_progress((uint8_t)(percent > 99 ? 99 : percent));  // 100 is "done": the file is not in place yet
    }
}

static p3a_format_t format_of(const char *path)
{
    uint8_t head[32];
    FILE *f = fopen(path, "rb");
    const size_t n = f != NULL ? fread(head, 1, sizeof(head), f) : 0;
    if (f != NULL) {
        fclose(f);
    }
    return (p3a_format_t)animation_player_format_of(head, n);
}

static esp_err_t show_job(void *arg)
{
    char *url = arg;
    char dir[160], part[200], name[P3A_RECORD_FILENAME_LEN], path[P3A_FS_PATH_MAX];
    esp_err_t err = ESP_OK;
    if (sd_health_is_failed() || p3a_storage_is_handed_over()) {
        err = P3A_ERR_SD_FAILED;
    } else if (p3a_mem_level() != P3A_MEM_OK) {
        err = ESP_ERR_NO_MEM;  // a new TLS session is optional work
    }
    if (err == ESP_OK) {
        err = sd_path_get_temporary(dir, sizeof(dir));
    }
    if (err == ESP_OK) {
        // Its own file: the content task may be downloading at the same moment.
        snprintf(part, sizeof(part), "%s/show.part", dir);
        err = sd_path_ensure_parent_dirs(part);
    }
    if (err == ESP_OK) {
        const http_fetch_request_t req = {.url = url,
                                          .user_agent = "p3a",
                                          .max_size = P3A_MAX_ARTWORK_BYTES,
                                          .leave_temp = true,
                                          .treat_empty_as_not_found = true,
                                          .wait_on_sdio = true,
                                          .progress = on_progress};
        err = http_fetch_to_file(&req, part, NULL);
    }
    p3a_format_t format = P3A_FORMAT_UNKNOWN;
    if (err == ESP_OK) {
        format = format_of(part);
        err = format == P3A_FORMAT_UNKNOWN ? (esp_err_t)P3A_ERR_NOT_PLAYABLE : show_url_name(url, format, name, sizeof(name));
    }
    if (err == ESP_OK) {
        err = sd_path_get_animations(dir, sizeof(dir));
    }
    if (err == ESP_OK) {
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        err = sd_path_ensure_parent_dirs(path);
    }
    if (err == ESP_OK) {
        // Moved into place whole. A file of that name already there is the
        // same address shown before: the new picture takes its place.
        err = fs_atomic_rename(part, path, NULL);
    }
    if (err != ESP_OK) {
        unlink(part);  // ours, in the temporary folder
        ESP_LOGW(TAG, "not shown: %s", p3a_err_to_name(err));  // the address is not logged: it may carry a key
    } else {
        p3a_record_t record;
        const bool playing = source_local_record(name, (uint32_t)time(NULL), &record) == ESP_OK &&
                             play_scheduler_show(&record) == ESP_OK;  // refused while paused: saved all the same
        (void)play_scheduler_refresh_source(P3A_SOURCE_LOCAL);
        ESP_LOGI(TAG, "saved as %s%s", name, playing ? " and playing" : "");
    }
    free(url);
    return err;
}

esp_err_t play_scheduler_show_url(const char *url, p3a_job_id_t *job)
{
    if (!show_url_ok(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strlen(url) + 1;
    char *copy = psram_malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, url, len);
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "show_url", show_job, copy, job);
    if (err != ESP_OK) {
        free(copy);
    }
    return err;
}
