// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution_download.c
 * @brief IIIF image download for institution channels
 *
 * Mirrors components/giphy/giphy_download.c — chunked write with retry on
 * truncated reads, atomic temp-file rename, SDIO-bus + USB-MSC export
 * awareness. URL and target path are passed in (built by the download
 * manager via art_institution_build_iiif_url +
 * art_institution_build_vault_path_from_spec), so this function does no
 * URL/path construction itself.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "http_fetch.h"
#include "download_manager.h"  // download_manager_is_canceled (S1 cooperative cancel)
#include "p3a_limits.h"
#include "sd_path.h"
#include "sdio_bus.h"
#include "makapix_channel_events.h"  // makapix_channel_is_sd_available
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "ai_dl";

#define DOWNLOAD_CHUNK_SIZE      (32 * 1024)
#define DOWNLOAD_MAX_REDIRECTS   5

// Cooperative abort: stop mid-download if the SD card got exported to USB or
// the playset switched. Mirrors the per-chunk inline checks the old loop did.
static bool ai_dl_should_abort(void *ctx)
{
    (void)ctx;
    if (!makapix_channel_is_sd_available()) {
        ESP_LOGI(TAG, "Aborting IIIF download: SD exported to USB");
        return true;
    }
    if (download_manager_is_canceled()) {
        ESP_LOGI(TAG, "Aborting IIIF download: playset switched");
        return true;
    }
    return false;
}

// 429 handler: engage the museum's cooldown (Retry-After honored if present).
// museum_id arrives via user_ctx since it's per-call, not a compile-time literal.
static void ai_dl_on_rate_limited(uint32_t retry_after_sec, void *ctx)
{
    const char *museum_id = (const char *)ctx;
    ESP_LOGW(TAG, "IIIF 429 for museum '%s' (Retry-After %us)",
             museum_id ? museum_id : "?", (unsigned)retry_after_sec);
    if (museum_id) art_institution_set_rate_limited(museum_id, retry_after_sec);
}

esp_err_t art_institution_download_to_path(const char *museum_id,
                                           const char *url,
                                           const char *out_path)
{
    if (!museum_id || !url || !out_path || url[0] == '\0' || out_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (art_institution_is_rate_limited(museum_id)) {
        ESP_LOGW(TAG, "Skipping IIIF download: museum '%s' rate-limited", museum_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Wait for SDIO bus if locked (e.g. animation prefetch).
    if (sdio_bus_is_locked()) {
        const char *holder = sdio_bus_get_holder();
        ESP_LOGD(TAG, "SDIO bus locked by %s, waiting...", holder ? holder : "?");
        int wait_count = 0;
        while (sdio_bus_is_locked() && wait_count < 120) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            wait_count++;
        }
        if (wait_count >= 120) return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = sd_path_ensure_parent_dirs(out_path);
    if (err != ESP_OK) return err;

    ESP_LOGD(TAG, "Downloading: %s -> %s", url, out_path);

    // HAM serves IIIF via an NRS URN (nrs.harvard.edu) that 303s to the IDS
    // image server (ids.lib.harvard.edu); the helper follows the chain
    // manually (capturing Location), resetting the temp file per hop. Museums
    // that don't redirect (AIC, V&A, Wellcome, SMK, Rijks micrio) hit a 200 on
    // the first hop. The User-Agent is required by the Smithsonian F5 WAF and
    // harmless to every other museum CDN.
    http_fetch_request_t fr = {
        .url = url,
        .redirect_mode = HTTP_FETCH_REDIRECT_MANUAL,
        .max_redirects = DOWNLOAD_MAX_REDIRECTS,
        .user_agent = ai_user_agent(),
        .max_size = P3A_MAX_ARTWORK_SIZE,
        .chunk_size = DOWNLOAD_CHUNK_SIZE,
        .treat_empty_as_not_found = true,
        .should_abort = ai_dl_should_abort,
        .on_rate_limited = ai_dl_on_rate_limited,
        .user_ctx = (void *)museum_id,
    };
    err = http_fetch_to_file(&fr, out_path, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IIIF download failed for %s: %s", url, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "Downloaded %s -> %s", url, out_path);
    return ESP_OK;
}
