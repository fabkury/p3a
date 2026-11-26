// SPDX-License-Identifier: Apache-2.0
// Installing a release: the image goes from the network straight into the
// other app slot, is checked, and the device restarts into it.
//
// Order of the checks, each before the next thing is touched:
//   1. The first bytes are those of a p3a image for this chip, of the
//      version the release said. Only then is the slot opened, so a wrong
//      answer (a web page, another product's file) never costs the firmware
//      that slot held.
//   2. The whole download has the SHA-256 GitHub holds for the asset.
//   3. The IDF verifies the image in the slot (esp_ota_end) and again when
//      the slot is made the one to boot.
// Whatever fails, the screen comes down and the running firmware carries on
// (1.x left its update screen up on two early failures: known issue H8).
//
// The flash calls that map memory run through p3a_internal_call(): this
// job's stack is in PSRAM.

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "p3a_err.h"
#include "p3a_internal_call.h"
#include "p3a_job.h"
#include "p3a_ui_hooks.h"
#include "psram_alloc.h"
#include "sdkconfig.h"
#include "update_internal.h"

static const char *TAG = "update";

// Where the app description sits in an image: after the image header and
// the first segment's header.
#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEAD_BYTES  (DESC_OFFSET + sizeof(esp_app_desc_t))

typedef struct {
    const update_release_t *release;
    const esp_partition_t *slot;
    esp_ota_handle_t handle;
    bool open;
    size_t written;
    int last_percent;
    char why[96];  // what was wrong, for a person
} install_t;

static esp_err_t slot_open(void *arg)
{
    install_t *in = arg;
    // Erased as it is written, not all 10 MB first.
    return esp_ota_begin(in->slot, OTA_WITH_SEQUENTIAL_WRITES, &in->handle);
}

static esp_err_t slot_abort(void *arg)
{
    install_t *in = arg;
    return esp_ota_abort(in->handle);
}

static esp_err_t slot_finish(void *arg)
{
    install_t *in = arg;
    esp_err_t err = esp_ota_end(in->handle);  // frees the handle whatever it answers
    in->open = false;
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(in->slot);
    }
    return err;
}

static void close_slot(install_t *in)
{
    if (in->open) {
        p3a_internal_call(slot_abort, in);
        in->open = false;
    }
}

// The body starts over (another attempt, or a redirect).
static esp_err_t on_restart(void *ctx)
{
    install_t *in = ctx;
    close_slot(in);
    in->written = 0;
    return ESP_OK;
}

static esp_err_t refuse(install_t *in, esp_err_t err, const char *why)
{
    snprintf(in->why, sizeof(in->why), "%s", why);
    return err;
}

// Is this the start of the image the release promised?
static esp_err_t check_head(install_t *in, const uint8_t *data, size_t len)
{
    if (len < HEAD_BYTES) {
        return refuse(in, P3A_ERR_CORRUPT, "the download is too short to be a firmware");
    }
    const esp_image_header_t *header = (const esp_image_header_t *)data;
    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        return refuse(in, P3A_ERR_CORRUPT, "the download is not a firmware image");
    }
    if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return refuse(in, P3A_ERR_CORRUPT, "the firmware is for another chip");
    }
    esp_app_desc_t desc;
    memcpy(&desc, data + DESC_OFFSET, sizeof(desc));
    desc.version[sizeof(desc.version) - 1] = '\0';
    desc.project_name[sizeof(desc.project_name) - 1] = '\0';
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        return refuse(in, P3A_ERR_CORRUPT, "the firmware is not p3a");
    }
    if (strcmp(desc.version, in->release->version) != 0) {
        snprintf(in->why, sizeof(in->why), "the firmware says it is version %.31s, not the release's", desc.version);
        return P3A_ERR_CORRUPT;
    }
    return ESP_OK;
}

static void show(const install_t *in, int percent, const char *what)
{
    p3a_ui_content_t words;
    p3a_ui_content(&words, "Updating", percent, "%s to %s. %s", g_update.running, in->release->version, what);
    p3a_ui_screen_show(P3A_SCREEN_UPDATE, &words);
}

static esp_err_t on_write(const uint8_t *data, size_t len, void *ctx)
{
    install_t *in = ctx;
    if (!in->open) {
        esp_err_t err = check_head(in, data, len);
        if (err != ESP_OK) {
            return err;
        }
        err = p3a_internal_call(slot_open, in);
        if (err != ESP_OK) {
            snprintf(in->why, sizeof(in->why), "the slot could not be opened (%s)", esp_err_to_name(err));
            return err;
        }
        in->open = true;
    }
    const esp_err_t err = esp_ota_write(in->handle, data, len);
    if (err != ESP_OK) {
        snprintf(in->why, sizeof(in->why), "writing to flash failed (%s)", esp_err_to_name(err));
        return err;
    }
    in->written += len;
    // Up to 95: the rest is the checks.
    const int percent = (int)((uint64_t)in->written * 95 / in->release->size);
    if (percent != in->last_percent) {
        in->last_percent = percent;
        p3a_job_set_progress((uint8_t)percent);
        update_lock();
        g_update.progress = (uint8_t)percent;
        update_unlock();
        show(in, percent, "Do not unplug");
    }
    return ESP_OK;
}

static esp_err_t install(install_t *in)
{
    const update_release_t *release = in->release;
    char agent[48];
    snprintf(agent, sizeof(agent), "p3a/%.31s", g_update.running);
    const http_fetch_request_t req = {
        .url = release->url,
        .user_agent = agent,
        .timeout_ms = 30000,
        .max_attempts = 2,
        // GitHub answers with a redirect to a signed address of about 900
        // characters: followed by hand, and the request line must hold it.
        .redirect_mode = HTTP_FETCH_REDIRECT_MANUAL,
        .tx_buffer_size = 2048,
        .max_size = release->size,
        .require_exact_length = true,
        .hash_file = true,
    };
    const http_fetch_stream_t stream = {.restart = on_restart, .write = on_write, .ctx = in};
    http_fetch_result_t result;
    esp_err_t err = http_fetch_to_stream(&req, &stream, &result);
    if (err != ESP_OK) {
        if (in->why[0] == '\0') {
            snprintf(in->why, sizeof(in->why), "the download failed (%s)", p3a_err_to_name(err));
        }
        return err;
    }
    if (result.bytes != release->size) {
        snprintf(in->why, sizeof(in->why), "the download has %u bytes; the release says %u", (unsigned)result.bytes,
                 (unsigned)release->size);
        return P3A_ERR_CORRUPT;
    }
    if (!result.has_sha256 || memcmp(result.sha256, release->sha256, sizeof(release->sha256)) != 0) {
        return refuse(in, P3A_ERR_CORRUPT, "the download does not match the release's checksum");
    }
    show(in, 97, "Checking");
    err = p3a_internal_call(slot_finish, in);
    if (err != ESP_OK) {
        snprintf(in->why, sizeof(in->why), "the firmware did not pass the check (%s)", esp_err_to_name(err));
    }
    return err;
}

esp_err_t update_install_job(void *arg)
{
    (void)arg;
    update_state_t *u = &g_update;
    update_release_t *release = psram_malloc(sizeof(*release));
    if (release == NULL) {
        update_lock();
        u->state = P3A_UPDATE_IDLE;
        u->install_error = ESP_ERR_NO_MEM;
        snprintf(u->install_error_text, sizeof(u->install_error_text), "out of memory");
        update_unlock();
        return ESP_ERR_NO_MEM;
    }
    update_lock();
    *release = u->latest;
    u->install_error = ESP_OK;
    u->install_error_text[0] = '\0';
    update_unlock();

    install_t in = {.release = release, .slot = u->other_slot, .last_percent = -1};
    ESP_LOGI(TAG, "installing %s (%u bytes) into %s", release->version, (unsigned)release->size, in.slot->label);
    show(&in, 0, "Do not unplug");
    const esp_err_t err = install(&in);
    close_slot(&in);  // if anything failed after the slot was opened

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s was not installed: %s", release->version, in.why);
        update_lock();
        u->state = P3A_UPDATE_IDLE;
        u->progress = 0;
        u->install_error = err;
        memcpy(u->install_error_text, in.why, sizeof(u->install_error_text));
        update_unlock();
        p3a_ui_screen_hide(P3A_SCREEN_UPDATE);
        free(release);
        return err;
    }

    ESP_LOGW(TAG, "%s is installed; restarting into it", release->version);
    update_lock();
    u->state = P3A_UPDATE_RESTARTING;
    u->progress = 100;
    update_unlock();
    p3a_job_set_progress(100);
    show(&in, 100, "Restarting");
    free(release);
    vTaskDelay(pdMS_TO_TICKS(2500));  // long enough for a page that polls to see "restarting"
    esp_restart();                    // asked for by a person, not a remedy: not counted
    return ESP_OK;
}
