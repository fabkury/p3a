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
#include "p3a_blob.h"
#include "p3a_err.h"
#include "p3a_limits.h"
#include "p3a_mem.h"
#include "p3a_ui_hooks.h"
#include "p3a_source.h"
#include "p3a_storage.h"
#include "play_scheduler.h"
#include "psram_alloc.h"
#include "sd_health.h"
#include "sd_path.h"
#include "source_local.h"

static const char *TAG = "show_url";

// The job's progress, and the same on the screen: a bar over what is
// playing, for a picture someone asked for and is waiting to see.
static void on_progress(size_t total, size_t content_length, void *ctx)
{
    int *shown = ctx;  // the last percentage put on the screen, -1 before the first
    if (content_length > 0) {
        const size_t percent = total * 100 / content_length;
        p3a_job_set_progress((uint8_t)(percent > 99 ? 99 : percent));  // 100 is "done": the file is not in place yet
        if (shown != NULL && (int)percent >= *shown + 5) {
            *shown = (int)percent;
            p3a_ui_content_t words;
            p3a_ui_content(&words, "Fetching a picture", (int)(percent > 100 ? 100 : percent), "%s", "");
            // Taken down when the fetch ends; the time limit is for a transfer that stalls.
            p3a_ui_notice_show(P3A_NOTICE_DOWNLOADING, &words, 20000, 0);
        }
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
    int shown = -1;  // the percentage last put on the screen
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
                                          .progress = on_progress,
                                          .user_ctx = &shown};
        err = http_fetch_to_file(&req, part, NULL);
        p3a_ui_notice_hide(P3A_NOTICE_DOWNLOADING);
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

// --- one artwork of a source --------------------------------------------------------

static esp_err_t artwork_job(void *arg)
{
    p3a_record_t *record = arg;
    esp_err_t err = ESP_OK;
    int shown = -1;  // the percentage last put on the screen
    // The device may have it already: it plays in one of the active
    // playset's channels and was fetched for it.
    if (!p3a_record_has_blob(record) && !play_scheduler_known(record, record)) {
        const p3a_source_ops_t *ops = p3a_source_get(record->source);
        char *url = psram_malloc(SHOW_URL_MAX_LEN);
        char dir[160], part[200];
        if (ops == NULL || ops->url == NULL) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else if (url == NULL) {
            err = ESP_ERR_NO_MEM;
        } else if (sd_health_is_failed() || p3a_storage_is_handed_over()) {
            err = P3A_ERR_SD_FAILED;
        } else if (p3a_mem_level() != P3A_MEM_OK) {
            err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK) {
            err = ops->url(record, url, SHOW_URL_MAX_LEN);
        }
        if (err == ESP_OK) {
            err = sd_path_get_temporary(dir, sizeof(dir));
        }
        http_fetch_result_t result = {0};
        if (err == ESP_OK) {
            snprintf(part, sizeof(part), "%s/show.part", dir);
            err = sd_path_ensure_parent_dirs(part);
        }
        if (err == ESP_OK) {
            const http_fetch_request_t req = {.url = url,
                                              .user_agent = ops->user_agent ? ops->user_agent : "p3a",
                                              .max_size = P3A_MAX_ARTWORK_BYTES,
                                              .leave_temp = true,  // the blob store moves it into place
                                              .redirect_mode = ops->redirects ? HTTP_FETCH_REDIRECT_MANUAL
                                                                              : HTTP_FETCH_REDIRECT_AUTO,
                                              .max_redirects = 5,
                                              .treat_empty_as_not_found = true,
                                              .wait_on_sdio = true,
                                              .hash_file = true,
                                              .progress = on_progress,
                                              .user_ctx = &shown};
            err = http_fetch_to_file(&req, part, &result);
            p3a_ui_notice_hide(P3A_NOTICE_DOWNLOADING);
        }
        if (err == ESP_OK) {
            const p3a_format_t format = format_of(part);
            err = format == P3A_FORMAT_UNKNOWN || !result.has_sha256
                      ? (esp_err_t)P3A_ERR_NOT_PLAYABLE
                      : p3a_blob_adopt_hashed(part, format, result.sha256, record->blob_ref, NULL);
            if (err == ESP_OK) {
                record->format = (uint8_t)format;
            } else {
                unlink(part);
            }
        }
        free(url);
    }
    if (err == ESP_OK) {
        err = play_scheduler_show(record);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "an artwork of %s was not shown: %s", p3a_source_name(record->source), p3a_err_to_name(err));
    }
    free(record);
    return err;
}

esp_err_t play_scheduler_show_artwork(const p3a_record_t *record, p3a_job_id_t *job)
{
    if (record == NULL || p3a_record_check(record) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    p3a_record_t *copy = psram_malloc(sizeof(*copy));
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *copy = *record;
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "show_artwork", artwork_job, copy, job);
    if (err != ESP_OK) {
        free(copy);
    }
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
