// SPDX-License-Identifier: Apache-2.0
// Firmware updates: what runs and what the other slot holds, looking for
// releases, the trial of a new firmware, going back. See p3a_update.h.
// The install itself is update_install.c.

#include "p3a_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_fetch.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_event_types.h"
#include "p3a_http.h"
#include "p3a_internal_call.h"
#include "p3a_mem.h"
#include "p3a_net.h"
#include "p3a_recovery.h"
#include "p3a_settings.h"
#include "p3a_storage.h"
#include "p3a_ui_hooks.h"
#include "psram_alloc.h"
#include "update_internal.h"
#include "update_policy.h"

static const char *TAG = "update";

#define TICK_US (10 * 1000 * 1000)

update_state_t g_update;

static uint32_t seconds(int64_t us)
{
    return (uint32_t)(us / 1000000);
}

// --- at boot ------------------------------------------------------------------------

static bool slot_version(const esp_partition_t *slot, char *out, size_t out_len)
{
    esp_app_desc_t desc;
    if (slot == NULL || esp_ota_get_partition_description(slot, &desc) != ESP_OK) {
        return false;
    }
    desc.version[sizeof(desc.version) - 1] = '\0';
    snprintf(out, out_len, "%s", desc.version);
    return true;
}

void p3a_update_boot(void)
{
    update_state_t *u = &g_update;
    u->lock = xSemaphoreCreateMutex();
    snprintf(u->running, sizeof(u->running), "%s", esp_app_get_description()->version);
    snprintf(u->list_url, sizeof(u->list_url), "%s", UPDATE_LIST_URL);
    snprintf(u->url_prefix, sizeof(u->url_prefix), "%s", UPDATE_URL_PREFIX);
    u->auto_check = true;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        u->on_trial = state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    u->other_slot = esp_ota_get_next_update_partition(NULL);
    // A firmware that was tried and left: the bootloader marked it.
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid != NULL) {
        slot_version(invalid, u->undone, sizeof(u->undone));
    }
    if (u->other_slot != NULL && u->other_slot != invalid) {
        esp_ota_img_states_t other_state = ESP_OTA_IMG_UNDEFINED;
        const bool has_state = esp_ota_get_state_partition(u->other_slot, &other_state) == ESP_OK;
        const bool refused = has_state && (other_state == ESP_OTA_IMG_INVALID || other_state == ESP_OTA_IMG_ABORTED);
        // A slot that was never booted from has no state: it is offered only
        // if what it holds is a whole p3a image (after a USB flash it is empty).
        if (!refused && slot_version(u->other_slot, u->other, sizeof(u->other))) {
            esp_app_desc_t desc;
            u->other_bootable = esp_ota_get_partition_description(u->other_slot, &desc) == ESP_OK &&
                                strncmp(desc.project_name, esp_app_get_description()->project_name,
                                        sizeof(desc.project_name)) == 0;
            if (!u->other_bootable) {
                u->other[0] = '\0';
            }
        }
    }
    ESP_LOGI(TAG, "running %s%s; other slot: %s; undone: %s", u->running, u->on_trial ? " (on trial)" : "",
             u->other[0] ? u->other : "nothing", u->undone[0] ? u->undone : "nothing");
}

// --- looking for a release ----------------------------------------------------------

static void read_settings(void)
{
    p3a_settings_t *settings = psram_malloc(sizeof(*settings));
    if (settings == NULL) {
        return;
    }
    p3a_settings_get(settings);
    update_lock();
    g_update.auto_check = settings->update_auto_check;
    g_update.prereleases = settings->update_prereleases;
    update_unlock();
    free(settings);
}

static esp_err_t look(update_release_t *release, bool *found)
{
    update_state_t *u = &g_update;
    *found = false;
    if (p3a_mem_level() != P3A_MEM_OK) {
        return ESP_ERR_NO_MEM;  // the list and its parse tree need a megabyte for a moment
    }
    char *list = psram_malloc(UPDATE_LIST_MAX);
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char url[sizeof(u->list_url)], prefix[sizeof(u->url_prefix)], agent[48];
    update_lock();
    memcpy(url, u->list_url, sizeof(url));
    memcpy(prefix, u->url_prefix, sizeof(prefix));
    const bool prereleases = u->prereleases;
    update_unlock();
    snprintf(agent, sizeof(agent), "p3a/%.31s", u->running);  // GitHub refuses a request without one
    static const http_fetch_header_t headers[] = {
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
    };
    const http_fetch_request_t req = {
        .url = url,
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .user_agent = agent,
        .timeout_ms = 20000,
        .max_attempts = 2,
    };
    size_t len = 0;
    esp_err_t err = http_fetch_to_buffer(&req, list, UPDATE_LIST_MAX, &len, NULL);
    if (err == ESP_OK) {
        const update_want_t want = {
            .prereleases = prereleases,
            .max_bytes = u->other_slot != NULL ? u->other_slot->size : 0,
            .url_prefix = prefix,
        };
        int considered = 0;
        err = update_release_pick(list, len, &want, release, &considered);
        if (err == ESP_OK) {
            *found = true;
            ESP_LOGI(TAG, "%d releases listed; the newest for this device is %s", considered, release->version);
        } else if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;  // a fine answer: nothing for this device
            ESP_LOGI(TAG, "%d releases listed; none for this device", considered);
        }
    }
    free(list);
    return err;
}

static esp_err_t check_job(void *arg)
{
    (void)arg;
    update_state_t *u = &g_update;
    update_release_t *release = psram_calloc(1, sizeof(*release));
    bool found = false;
    const esp_err_t err = release != NULL ? look(release, &found) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not look for releases: %s", p3a_err_to_name(err));
    }
    update_lock();
    u->checked = true;
    u->check_ok = err == ESP_OK;
    u->check_error = err;
    u->checked_us = esp_timer_get_time();
    if (err == ESP_OK) {
        u->has_latest = found;
        if (found) {
            u->latest = *release;
        }
    }
    u->state = P3A_UPDATE_IDLE;
    update_unlock();
    free(release);
    return err;
}

// Takes the state from IDLE to `to` if it is IDLE.
static bool claim(p3a_update_state_t to)
{
    bool mine = false;
    update_lock();
    if (g_update.state == P3A_UPDATE_IDLE) {
        g_update.state = to;
        g_update.progress = 0;
        mine = true;
    }
    update_unlock();
    return mine;
}

static void release_claim(void)
{
    update_lock();
    g_update.state = P3A_UPDATE_IDLE;
    update_unlock();
}

esp_err_t p3a_update_check(p3a_job_id_t *out_job)
{
    if (g_update.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim(P3A_UPDATE_CHECKING)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "update check", check_job, NULL, out_job);
    if (err != ESP_OK) {
        release_claim();
    }
    return err;
}

static bool newer_known(void)
{
    const update_state_t *u = &g_update;
    return u->has_latest && update_version_compare(u->latest.version, u->running) > 0;
}

esp_err_t p3a_update_install(p3a_job_id_t *out_job)
{
    if (g_update.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (p3a_storage_is_handed_over()) {
        return P3A_ERR_SD_FAILED;
    }
    update_lock();
    const bool have = newer_known() && g_update.other_slot != NULL;
    update_unlock();
    if (!have) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!claim(P3A_UPDATE_INSTALLING)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "update install", update_install_job, NULL, out_job);
    if (err != ESP_OK) {
        release_claim();
    }
    return err;
}

// --- going back, and the trial --------------------------------------------------------

static esp_err_t boot_other(void *arg)
{
    return esp_ota_set_boot_partition((const esp_partition_t *)arg);  // verifies the image first
}

static esp_err_t give_up_trial(void *arg)
{
    (void)arg;
    return esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return when it works
}

static esp_err_t keep_trial(void *arg)
{
    (void)arg;
    return esp_ota_mark_app_valid_cancel_rollback();
}

static esp_err_t roll_back_job(void *arg)
{
    (void)arg;
    update_state_t *u = &g_update;
    p3a_ui_content_t words;
    p3a_ui_content(&words, "Going back", -1, "%s to %s. Do not unplug", u->running, u->other);
    p3a_ui_screen_show(P3A_SCREEN_UPDATE, &words);
    // A firmware on trial goes back by being refused; a kept one by choosing the other slot.
    esp_err_t err = u->on_trial ? p3a_internal_call(give_up_trial, NULL)
                                : p3a_internal_call(boot_other, (void *)u->other_slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not go back to %s: %s", u->other, esp_err_to_name(err));
        p3a_ui_screen_hide(P3A_SCREEN_UPDATE);
        release_claim();
        return err;
    }
    ESP_LOGW(TAG, "going back to %s on request; restarting", u->other);
    vTaskDelay(pdMS_TO_TICKS(1500));  // time for the answer to reach the page
    esp_restart();                    // asked for by a person, not a remedy: not counted
    return ESP_OK;
}

esp_err_t p3a_update_roll_back(p3a_job_id_t *out_job)
{
    if (g_update.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_update.other[0] == '\0' || g_update.other_slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!claim(P3A_UPDATE_RESTARTING)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = p3a_job_submit(P3A_JOBS_IO, "update roll back", roll_back_job, NULL, out_job);
    if (err != ESP_OK) {
        release_claim();
    }
    return err;
}

static esp_err_t trial_job(void *arg)
{
    const bool good = arg != NULL;
    if (good) {
        const esp_err_t err = p3a_internal_call(keep_trial, NULL);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s is reachable: kept", g_update.running);
            p3a_recovery_clear();  // the restarts the firmware before this one needed are its own
            update_lock();
            g_update.on_trial = false;
            update_unlock();
        } else {
            ESP_LOGE(TAG, "could not mark this firmware as kept: %s", esp_err_to_name(err));
        }
        return err;
    }
    // Not a remedy that could repeat: the firmware gone back to is not on trial.
    ESP_LOGE(TAG, "%s could not be reached %u minutes after it started: going back to %s", g_update.running,
             (unsigned)(UPDATE_TRIAL_LIMIT_S / 60), g_update.other[0] ? g_update.other : "the firmware before");
    p3a_ui_content_t words;
    p3a_ui_content(&words, "Update undone", -1, "%s could not reach the network. Going back", g_update.running);
    p3a_ui_screen_show(P3A_SCREEN_UPDATE, &words);
    vTaskDelay(pdMS_TO_TICKS(3000));
    const esp_err_t err = p3a_internal_call(give_up_trial, NULL);
    ESP_LOGE(TAG, "going back failed: %s", esp_err_to_name(err));
    p3a_ui_screen_hide(P3A_SCREEN_UPDATE);
    return err;
}

// --- the clock --------------------------------------------------------------------------

static void tick(void *arg)
{
    (void)arg;
    update_state_t *u = &g_update;
    p3a_net_status_t net;
    p3a_net_status(&net);
    const int64_t now = esp_timer_get_time();
    const bool up = net.state == P3A_NET_UP;

    update_lock();
    if (up && !u->net_seen) {
        u->net_seen = true;
        u->net_up_us = now;
    }
    const update_clock_t clock = {
        .enabled = u->auto_check,
        .net_up = up,
        .busy = u->state != P3A_UPDATE_IDLE || p3a_storage_is_handed_over(),
        .checked = u->checked,
        .last_ok = u->check_ok,
        .now_s = seconds(now),
        .net_up_s = seconds(u->net_up_us),
        .net_seen = u->net_seen,
        .last_s = seconds(u->checked_us),
    };
    update_trial_t verdict = UPDATE_TRIAL_WAIT;
    if (u->on_trial && !u->trial_decided) {
        const update_trial_facts_t facts = {
            .uptime_s = seconds(now),
            .boot_done = u->boot_done,
            .has_credentials = net.has_credentials,
            .net_up = up,
            .http_up = p3a_http_is_up(),
        };
        verdict = update_trial_verdict(&facts);
        // Never give up a firmware in the middle of what its owner asked of it.
        if (verdict == UPDATE_TRIAL_BAD && u->state != P3A_UPDATE_IDLE) {
            verdict = UPDATE_TRIAL_WAIT;
        }
        u->trial_decided = verdict != UPDATE_TRIAL_WAIT;
    }
    update_unlock();

    if (verdict != UPDATE_TRIAL_WAIT) {
        if (p3a_job_submit(P3A_JOBS_IO, "update trial", trial_job, verdict == UPDATE_TRIAL_GOOD ? (void *)1 : NULL,
                           NULL) != ESP_OK) {
            update_lock();
            u->trial_decided = false;  // the runner was full: decide again at the next tick
            update_unlock();
        }
    }
    if (update_check_due(&clock)) {
        p3a_update_check(NULL);
    }
}

static void on_setting_changed(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    read_settings();
}

esp_err_t p3a_update_start(void)
{
    if (g_update.lock == NULL) {
        return ESP_ERR_INVALID_STATE;  // p3a_update_boot() did not run
    }
    read_settings();
    p3a_event_subscribe(P3A_EVENT_SETTING_CHANGED, on_setting_changed, NULL);
    esp_err_t err = update_api_register();
    if (err != ESP_OK) {
        return err;
    }
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = tick, .name = "update_tick"};
    err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(timer, TICK_US);
    }
    return err;
}

void p3a_update_boot_done(void)
{
    if (g_update.lock == NULL) {
        return;
    }
    update_lock();
    g_update.boot_done = true;
    update_unlock();
}

void p3a_update_status(p3a_update_status_t *out)
{
    memset(out, 0, sizeof(*out));
    const update_state_t *u = &g_update;
    if (u->lock == NULL) {
        return;
    }
    update_lock();
    out->state = u->state;
    out->progress = u->progress;
    memcpy(out->running, u->running, sizeof(out->running));
    out->on_trial = u->on_trial;
    memcpy(out->other, u->other, sizeof(out->other));
    memcpy(out->undone, u->undone, sizeof(out->undone));
    out->checked = u->checked;
    out->checked_ago_s = u->checked ? seconds(esp_timer_get_time() - u->checked_us) : 0;
    out->check_error = u->check_error;
    out->has_latest = u->has_latest;
    if (u->has_latest) {
        out->available = newer_known();
        memcpy(out->latest, u->latest.version, sizeof(out->latest));
        out->latest_prerelease = u->latest.prerelease;
        out->latest_bytes = u->latest.size;
        memcpy(out->latest_published, u->latest.published, sizeof(out->latest_published));
        memcpy(out->latest_notes, u->latest.notes, sizeof(out->latest_notes));
    }
    out->install_error = u->install_error;
    memcpy(out->install_error_text, u->install_error_text, sizeof(out->install_error_text));
    update_unlock();
    out->blocked = p3a_storage_is_handed_over() ? "CARD_IN_USE" : NULL;
}

void p3a_update_set_source(const char *list_url, const char *download_prefix)
{
    if (g_update.lock == NULL) {
        return;
    }
    update_lock();
    snprintf(g_update.list_url, sizeof(g_update.list_url), "%s", list_url != NULL ? list_url : UPDATE_LIST_URL);
    snprintf(g_update.url_prefix, sizeof(g_update.url_prefix), "%s",
             download_prefix != NULL ? download_prefix : UPDATE_URL_PREFIX);
    g_update.has_latest = false;
    g_update.checked = false;
    update_unlock();
}
