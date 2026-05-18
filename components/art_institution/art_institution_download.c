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
#define DOWNLOAD_MAX_ATTEMPTS    3
#define DOWNLOAD_MAX_REDIRECTS   5

static const uint32_t s_backoff_ms[DOWNLOAD_MAX_ATTEMPTS] = { 0, 1000, 3000 };

// Per-request scratch the event handler writes into. ESP-IDF's HTTP client
// does not reliably expose the Location header via get_header() when the
// open/fetch_headers/read pattern is used (observed on IDF v5.5.2 and
// documented in museums/rijksmuseum.c — the rijks JSON-LD path needs the
// same workaround). The ON_HEADER event fires synchronously during header
// parsing, so we capture Location into this scratch struct and drive the
// redirect manually in the outer loop. HAM is the immediate consumer
// (nrs.harvard.edu → ids.lib.harvard.edu 303 per
// docs/art-institutions/ham-investigation/REPORT.md §"Storage strategy").
typedef struct {
    char location[512];
} ai_dl_redirect_ctx_t;

static esp_err_t ai_dl_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    ai_dl_redirect_ctx_t *ctx = (ai_dl_redirect_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;
    if (evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
        strlcpy(ctx->location, evt->header_value, sizeof(ctx->location));
    }
    return ESP_OK;
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

    char temp_path[264];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", out_path);

    ESP_LOGD(TAG, "Downloading: %s -> %s", url, out_path);

    uint8_t *chunk = heap_caps_malloc(DOWNLOAD_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!chunk) {
        chunk = malloc(DOWNLOAD_CHUNK_SIZE);
        if (!chunk) return ESP_ERR_NO_MEM;
    }

    // Redirect chain handling: HAM serves IIIF via an NRS URN
    // (`nrs.harvard.edu`) that 303s to the IDS image server
    // (`ids.lib.harvard.edu`). ESP-IDF's HTTP client does not reliably
    // auto-follow 3xx when using open/fetch_headers/read, so we walk the
    // chain manually with a small cap. Museums that don't need redirects
    // (AIC, V&A, Wellcome, SMK, Rijks' micrio downloads) hit a 200 on the
    // first hop and the redirect counter never increments.
    char current_url[512];
    strlcpy(current_url, url, sizeof(current_url));

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open temp file: %s", temp_path);
        free(chunk);
        return ESP_FAIL;
    }

    bool success = false;
    esp_err_t fatal_err = ESP_OK;
    size_t total_written = 0;

    for (int redirect_hop = 0;
         redirect_hop <= DOWNLOAD_MAX_REDIRECTS && !success && fatal_err == ESP_OK;
         redirect_hop++) {

        ai_dl_redirect_ctx_t redir_ctx = {0};
        esp_http_client_config_t cfg = {
            .url = current_url,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
            .disable_auto_redirect = true,
            .event_handler = ai_dl_event_handler,
            .user_data = &redir_ctx,
        };

        bool got_redirect = false;
        char next_url[512] = "";

        for (int attempt = 0; attempt < DOWNLOAD_MAX_ATTEMPTS && !success && !got_redirect && fatal_err == ESP_OK; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "Retrying %s in %lums (attempt %d/%d)",
                         out_path, (unsigned long)s_backoff_ms[attempt],
                         attempt + 1, DOWNLOAD_MAX_ATTEMPTS);
                vTaskDelay(pdMS_TO_TICKS(s_backoff_ms[attempt]));
                rewind(f);
                if (ftruncate(fileno(f), 0) != 0) {
                    fatal_err = ESP_FAIL;
                    break;
                }
                total_written = 0;
            }

            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (!client) continue;

            esp_err_t open_err = esp_http_client_open(client, 0);
            if (open_err != ESP_OK) {
                ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(open_err));
                esp_http_client_cleanup(client);
                continue;
            }

            esp_http_client_fetch_headers(client);
            int64_t content_length = esp_http_client_get_content_length(client);
            int status = esp_http_client_get_status_code(client);

            if (status == 404) {
                ESP_LOGW(TAG, "IIIF 404: %s", current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_NOT_FOUND;
                break;
            }
            if (status == 429) {
                art_institution_set_rate_limited(museum_id, 0);
                ESP_LOGW(TAG, "IIIF 429: %s", current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (status >= 300 && status < 400) {
                if (redir_ctx.location[0]) {
                    strlcpy(next_url, redir_ctx.location, sizeof(next_url));
                    got_redirect = true;
                    ESP_LOGD(TAG, "IIIF redirect %d (HTTP %d): %.80s -> %.80s",
                             redirect_hop + 1, status, current_url, next_url);
                } else {
                    ESP_LOGW(TAG, "IIIF %d for %.80s but no Location header",
                             status, current_url);
                    fatal_err = ESP_FAIL;
                }
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                break;
            }
            if (status != 200) {
                ESP_LOGW(TAG, "HTTP %d for %s", status, current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                continue;  // retry
            }
            if (content_length == 0) {
                // HTTP 200 with explicit Content-Length: 0 means the origin is
                // serving an empty body for this resource — treat as permanent
                // not-found so download_manager creates a .404 marker and we
                // stop re-attempting this URL on every channel rotation.
                ESP_LOGW(TAG, "IIIF empty body (200 with Content-Length: 0): %s", current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_NOT_FOUND;
                break;
            }
            if (content_length > P3A_MAX_ARTWORK_SIZE) {
                ESP_LOGW(TAG, "IIIF artwork too large: %lld bytes (%s)",
                         content_length, current_url);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                fatal_err = ESP_ERR_INVALID_SIZE;
                break;
            }

            bool read_ok = true;
            bool write_err = false;
            bool too_large = false;

            while (true) {
                int chunk_received = 0;
                while (chunk_received < DOWNLOAD_CHUNK_SIZE) {
                    int n = esp_http_client_read(client,
                                                 (char *)chunk + chunk_received,
                                                 DOWNLOAD_CHUNK_SIZE - chunk_received);
                    if (n < 0) { read_ok = false; break; }
                    if (n == 0) break;
                    chunk_received += n;
                }
                if (!read_ok || chunk_received == 0) break;

                if (!makapix_channel_is_sd_available()) {
                    ESP_LOGI(TAG, "Aborting download of %s: SD exported to USB", current_url);
                    fatal_err = ESP_ERR_INVALID_STATE;
                    break;
                }

                size_t written = fwrite(chunk, 1, chunk_received, f);
                if (written != (size_t)chunk_received) {
                    write_err = true;
                    break;
                }
                total_written += written;

                if (total_written > P3A_MAX_ARTWORK_SIZE) {
                    too_large = true;
                    break;
                }
                if (chunk_received < DOWNLOAD_CHUNK_SIZE) break;  // last chunk
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (write_err) { fatal_err = ESP_FAIL; break; }
            if (too_large) { fatal_err = ESP_ERR_INVALID_SIZE; break; }

            bool truncated = !read_ok ||
                             total_written == 0 ||
                             (content_length > 0 && total_written < (size_t)content_length);
            if (truncated) {
                ESP_LOGW(TAG, "Truncated IIIF download for %s: got %zu/%lld bytes",
                         current_url, total_written, (long long)content_length);
                continue;  // retry
            }
            success = true;
        }  // end inner attempt loop

        // Advance to the next redirect hop if one was captured; otherwise
        // we're done (either success, fatal_err, or attempts exhausted on
        // a non-redirect response — the last is treated as ESP_FAIL so we
        // don't burn the redirect budget on the same URL).
        if (got_redirect && next_url[0] && !success && fatal_err == ESP_OK) {
            strlcpy(current_url, next_url, sizeof(current_url));
            // The temp file may have partial bytes from a prior attempt at
            // this hop (none yet on first hop; retries within a hop reset
            // already). Reset for the next hop's fresh download.
            rewind(f);
            if (ftruncate(fileno(f), 0) != 0) {
                fatal_err = ESP_FAIL;
            }
            total_written = 0;
            // Outer for-loop's increment handles redirect_hop; just continue.
        } else if (!success && fatal_err == ESP_OK) {
            // Inner attempts exhausted without a redirect and without success.
            // Stop here; outer loop's "max redirects" branch below would log
            // a misleading message in that case.
            break;
        }
    }  // end outer redirect loop

    if (fatal_err == ESP_OK && !success) {
        // Hit DOWNLOAD_MAX_REDIRECTS without resolving to a 200.
        ESP_LOGW(TAG, "Too many redirects (>%d) starting at %s",
                 DOWNLOAD_MAX_REDIRECTS, url);
        fatal_err = ESP_FAIL;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(chunk);

    if (fatal_err != ESP_OK) {
        unlink(temp_path);
        return fatal_err;
    }
    if (!success) {
        unlink(temp_path);
        ESP_LOGE(TAG, "Download failed for %s after %d attempts",
                 url, DOWNLOAD_MAX_ATTEMPTS);
        return ESP_FAIL;
    }

    unlink(out_path);  // remove any stale victim
    if (rename(temp_path, out_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: %s", temp_path, out_path, strerror(errno));
        unlink(temp_path);
        return ESP_FAIL;
    }

    // ESP_LOGD: the matching "dl_mgr: Downloading: ..." line is already
    // logged at INFO before this function runs, and "ps_lai: >>> LAi ADD"
    // confirms a successful landing right after. A third per-artwork
    // INFO message just crowded out the actually-useful events.
    ESP_LOGD(TAG, "Downloaded %s (%zu bytes) -> %s", url, total_written, out_path);
    return ESP_OK;
}
