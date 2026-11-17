// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_health.c
 * @brief SD card health tracking: boot probe + passive failure counting,
 *        latched (sticky-until-reboot) failure state. See sd_health.h.
 */

#include "sd_health.h"
#include "sd_path.h"
#include "event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "sd_health";

// Only paths under the SD mount prefix count toward SD health; LittleFS
// (/webui) writes must never trip the latch.
#define SD_MOUNT_PREFIX "/sdcard"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_failed = false;   // write-once latch
static bool s_probe_passed = false;
static bool s_overlay_pending = false;
static int s_consec_failures = 0;
static bool s_initialized = false;

// Idempotent: runs its body only on the first trip.
static void latch_failed(const char *reason)
{
    bool first = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_failed) {
        s_failed = true;
        s_overlay_pending = true;
        first = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!first) {
        return;
    }
    ESP_LOGE(TAG, "SD CARD FAILURE latched (%s) - writes/downloads disabled until reboot", reason);
    event_bus_emit_simple(P3A_EVENT_SD_HEALTH_FAILED);
}

static bool is_sd_path(const char *path)
{
    return path && strncmp(path, SD_MOUNT_PREFIX, strlen(SD_MOUNT_PREFIX)) == 0;
}

esp_err_t sd_health_init(void)
{
    s_initialized = true;
    return ESP_OK;
}

esp_err_t sd_health_boot_probe(void)
{
    if (s_failed) {
        return ESP_FAIL;  // e.g. mount already reported failed
    }

    const char *root = sd_path_get_root();
    char probe_path[SD_PATH_ROOT_MAX_LEN + 16];
    char tmp_path[SD_PATH_ROOT_MAX_LEN + 24];
    snprintf(probe_path, sizeof(probe_path), "%s/.sdh_probe", root);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", probe_path);

    // Clean up any stale probe files from a previous interrupted boot
    unlink(tmp_path);
    unlink(probe_path);

    // Fresh pattern each boot so read-back can't be satisfied by stale data
    char pattern[64];
    int pattern_len = snprintf(pattern, sizeof(pattern),
                               "p3a-sd-probe tick=%lu",
                               (unsigned long)xTaskGetTickCount());

    const char *step = NULL;
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        step = "open";
        goto fail;
    }
    if (fwrite(pattern, 1, pattern_len, f) != (size_t)pattern_len) {
        step = "write";
        fclose(f);
        unlink(tmp_path);
        goto fail;
    }
    fflush(f);
    if (fsync(fileno(f)) != 0) {
        step = "fsync";
        fclose(f);
        unlink(tmp_path);
        goto fail;
    }
    fclose(f);

    if (rename(tmp_path, probe_path) != 0) {
        step = "rename";
        unlink(tmp_path);
        goto fail;
    }

    char readback[sizeof(pattern)] = {0};
    f = fopen(probe_path, "rb");
    if (!f) {
        step = "reopen";
        goto fail;
    }
    size_t got = fread(readback, 1, sizeof(readback) - 1, f);
    fclose(f);
    if (got != (size_t)pattern_len || memcmp(readback, pattern, pattern_len) != 0) {
        step = "verify";
        goto fail;
    }

    unlink(probe_path);
    s_probe_passed = true;
    ESP_LOGI(TAG, "boot probe OK (%s)", probe_path);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "boot probe FAILED at %s (errno=%d)", step, errno);
    latch_failed("boot probe");
    return ESP_FAIL;
}

void sd_health_report_mount_failed(void)
{
    latch_failed("mount failed");
}

void sd_health_report_write_ok(const char *path)
{
    if (s_failed || !is_sd_path(path)) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_consec_failures = 0;
    portEXIT_CRITICAL(&s_lock);
}

void sd_health_report_write_failure(const char *path, int err_no)
{
    if (s_failed || !is_sd_path(path)) {
        return;
    }
    if (err_no == ENOSPC) {
        return;  // disk-full is not card failure
    }
    bool trip = false;
    int streak;
    portENTER_CRITICAL(&s_lock);
    streak = ++s_consec_failures;
    if (streak >= SD_HEALTH_FAIL_THRESHOLD) {
        trip = true;
    }
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "SD write failure %d/%d: %s (errno=%d)",
             streak, SD_HEALTH_FAIL_THRESHOLD, path, err_no);
    if (trip) {
        latch_failed("consecutive write failures");
    }
}

bool sd_health_is_failed(void)
{
    return s_failed;
}

sd_health_state_t sd_health_get_state(void)
{
    if (s_failed) {
        return SD_HEALTH_FAILED;
    }
    if (!s_initialized || !s_probe_passed) {
        return SD_HEALTH_UNKNOWN;
    }
    return SD_HEALTH_OK;
}

bool sd_health_take_pending_overlay(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_lock);
    if (s_overlay_pending) {
        s_overlay_pending = false;
        pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return pending;
}
