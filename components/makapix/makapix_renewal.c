// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_renewal.c
 * @brief Device-initiated MQTT client-certificate renewal
 *
 * See makapix_renewal.h for the design overview and
 * docs/makapix-cert-renewal/PLAN.md for the full plan (server contract
 * verified against the MPX source 2026-07-08).
 */

#include "makapix_internal.h"
#include "makapix_renewal.h"
#include "http_fetch.h"
#include "sntp_sync.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "cJSON.h"
#include "mbedtls/x509_crt.h"
#include <time.h>

// Poll cadence while waiting for the renewal prerequisites (Wi-Fi with an IP
// and an SNTP-synchronized clock) to come true.
#define RENEWAL_PREREQ_POLL_MS         (10 * 1000)

// Jitter spread applied to the window opening so the fleet doesn't hit the
// API in lockstep the day their (batch-issued) certs become renewable. Drawn
// fresh at every daily check, which smears renewals over roughly this range.
#define RENEWAL_JITTER_RANGE_S         (7 * 24 * 3600)

// Inside this many seconds of expiry the jitter is ignored: getting renewed
// outranks smoothing server load.
#define RENEWAL_NO_JITTER_MARGIN_S     (14 * 24 * 3600)

// Clock plausibility gate. mbedTLS date math and the window check are
// meaningless on an unsynced clock (epoch 1970), and a garbage clock must
// not trigger a fleet-wide renewal storm. This firmware did not exist
// before 2026, so any earlier time is not real.
#define RENEWAL_MIN_VALID_EPOCH        1767225600LL  // 2026-01-01T00:00:00Z

// Response sizing: renew-cert returns three JSON-escaped PEMs (~6-9 KB
// today), same shape as the provisioning credentials response.
#define RENEWAL_RESPONSE_BUF_SIZE      32768
#define ROTATE_RESPONSE_BUF_SIZE       1024

static const char *TAG = "makapix_renewal";

// PSRAM-backed stack for the renewal task (HTTP + cJSON over 3 PEMs)
#define RENEWAL_TASK_STACK_SIZE 12288
static StackType_t *s_renewal_stack = NULL;
static StaticTask_t s_renewal_task_buffer;
static TaskHandle_t s_renewal_task_handle = NULL;

// Serializes attempts (periodic task vs. self-heal from the reconnect task)
// and guards the status snapshot.
static SemaphoreHandle_t s_renewal_mutex = NULL;

static makapix_renewal_status_t s_status = {0};

// ---------------------------------------------------------------------------
// Certificate expiry (local, offline)
// ---------------------------------------------------------------------------

// Days-from-civil (Howard Hinnant's algorithm): UTC-only, no timezone or DST
// traps, valid across the whole X.509 date range. Returns days since
// 1970-01-01.
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static time_t x509_time_to_epoch(const mbedtls_x509_time *t)
{
    int64_t days = days_from_civil(t->year, (unsigned)t->mon, (unsigned)t->day);
    return (time_t)(days * 86400LL + t->hour * 3600 + t->min * 60 + t->sec);
}

/**
 * Parse the stored client certificate and return its notAfter as epoch UTC.
 */
static esp_err_t read_cert_not_after(time_t *out_not_after)
{
    char *cert_pem = malloc(MAKAPIX_PEM_MAX_LEN);
    if (!cert_pem) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = makapix_store_get_client_cert(cert_pem, MAKAPIX_PEM_MAX_LEN);
    if (err != ESP_OK) {
        free(cert_pem);
        return err;
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    // mbedTLS PEM parsing requires the length to include the NUL terminator.
    int mret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem,
                                      strlen(cert_pem) + 1);
    if (mret != 0) {
        ESP_LOGE(TAG, "Failed to parse stored client cert (mbedtls -0x%04x)", -mret);
        mbedtls_x509_crt_free(&crt);
        free(cert_pem);
        return ESP_FAIL;
    }

    *out_not_after = x509_time_to_epoch(&crt.valid_to);
    mbedtls_x509_crt_free(&crt);
    free(cert_pem);
    return ESP_OK;
}

static bool clock_is_plausible(void)
{
    return time(NULL) >= (time_t)RENEWAL_MIN_VALID_EPOCH;
}

static void status_set_result(const char *result)
{
    snprintf(s_status.last_result, sizeof(s_status.last_result), "%s", result);
}

// ---------------------------------------------------------------------------
// HTTP: token bootstrap and renewal
// ---------------------------------------------------------------------------

/**
 * POST /api/player/{player_key}/token/rotate
 *
 * Gated only on knowledge of the player_key; revokes the previous token.
 * Never auto-retried here: if the response is lost after a server-side
 * success, our stored token is dead — but the next renewal's 401 path
 * rotates again, so the failure mode is self-correcting.
 */
static esp_err_t rotate_api_token(const char *player_key,
                                  char *out_token, size_t out_token_len)
{
    char url[256];
    snprintf(url, sizeof(url), "https://%s/api/player/%s/token/rotate",
             CONFIG_MAKAPIX_CLUB_HOST, player_key);

    char *response_buf = malloc(ROTATE_RESPONSE_BUF_SIZE);
    if (!response_buf) {
        return ESP_ERR_NO_MEM;
    }

    http_fetch_request_t fr = {
        .url = url,
        .method = HTTP_FETCH_POST,
        .timeout_ms = 15000,
        .max_attempts = 1,
    };
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, ROTATE_RESPONSE_BUF_SIZE, NULL, &res);

    if (err != ESP_OK) {
        // 404 = player unknown / not registered: propagate so the caller can
        // treat the registration as genuinely dead.
        ESP_LOGE(TAG, "token/rotate failed: %s (HTTP %d)", esp_err_to_name(err), res.http_status);
        free(response_buf);
        return err;
    }

    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    cJSON *json = cJSON_Parse(response_buf);
    if (json) {
        cJSON *token = cJSON_GetObjectItem(json, "api_token");
        if (token && cJSON_IsString(token)) {
            const char *token_str = cJSON_GetStringValue(token);
            if (strlen(token_str) < out_token_len) {
                snprintf(out_token, out_token_len, "%s", token_str);
                result = ESP_OK;
            } else {
                ESP_LOGE(TAG, "api_token too large (%zu bytes)", strlen(token_str));
            }
        } else {
            ESP_LOGE(TAG, "token/rotate response missing api_token");
        }
        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Failed to parse token/rotate response");
    }

    free(response_buf);
    return result;
}

/**
 * Get a usable bearer token: stored one if present, otherwise rotate and
 * persist. The fresh token is persisted BEFORE this returns — rotation
 * revoked the previous token, so an unpersisted new token must not be the
 * only copy. An NVS write failure retries the write, never the rotate.
 */
static esp_err_t obtain_api_token(const char *player_key, bool force_rotate,
                                  char *out_token, size_t out_token_len)
{
    if (!force_rotate &&
        makapix_store_get_api_token(out_token, out_token_len) == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "%s bearer token via token/rotate",
             force_rotate ? "Refreshing" : "Bootstrapping");

    esp_err_t err = rotate_api_token(player_key, out_token, out_token_len);
    if (err != ESP_OK) {
        return err;
    }

    // Persist-before-rely: the previous token is already revoked server-side.
    esp_err_t save_err = makapix_store_set_api_token(out_token);
    if (save_err != ESP_OK) {
        // One retry; if NVS stays broken, still return OK — the in-RAM token
        // works for this attempt and the 401 path recovers next time.
        ESP_LOGE(TAG, "Failed to persist api_token (%s), retrying once",
                 esp_err_to_name(save_err));
        save_err = makapix_store_set_api_token(out_token);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "api_token persist retry failed (%s); continuing with in-RAM token",
                     esp_err_to_name(save_err));
        }
    }
    return ESP_OK;
}

/**
 * POST /api/player/renew-cert with the bearer token.
 *
 * On 200, parses and persists the returned cert/key/CA in one NVS commit.
 */
static esp_err_t call_renew_cert(const char *bearer_token, int *out_http_status)
{
    char url[192];
    snprintf(url, sizeof(url), "https://%s/api/player/renew-cert", CONFIG_MAKAPIX_CLUB_HOST);

    char auth_value[MAKAPIX_API_TOKEN_MAX_LEN + 8];
    snprintf(auth_value, sizeof(auth_value), "Bearer %s", bearer_token);

    char *response_buf = heap_caps_malloc(RENEWAL_RESPONSE_BUF_SIZE,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer from SPIRAM");
        return ESP_ERR_NO_MEM;
    }

    const http_fetch_header_t headers[] = {
        { .name = "Authorization", .value = auth_value },
    };
    http_fetch_request_t fr = {
        .url = url,
        .method = HTTP_FETCH_POST,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = 30000,
        .max_attempts = 1,
    };
    http_fetch_result_t res = {0};
    esp_err_t err = http_fetch_to_buffer(&fr, response_buf, RENEWAL_RESPONSE_BUF_SIZE, NULL, &res);
    if (out_http_status) {
        *out_http_status = res.http_status;
    }

    if (err != ESP_OK) {
        if (res.buffer_full) {
            ESP_LOGE(TAG, "renew-cert response exceeded %d-byte buffer", RENEWAL_RESPONSE_BUF_SIZE);
        } else {
            ESP_LOGW(TAG, "renew-cert failed: %s (HTTP %d)", esp_err_to_name(err), res.http_status);
        }
        free(response_buf);
        return err;
    }

    // Parse the three PEMs; all are required (ca_pem refreshes the trust
    // anchor in the same call).
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    cJSON *json = cJSON_Parse(response_buf);
    if (json) {
        cJSON *cert_pem = cJSON_GetObjectItem(json, "cert_pem");
        cJSON *key_pem = cJSON_GetObjectItem(json, "key_pem");
        cJSON *ca_pem = cJSON_GetObjectItem(json, "ca_pem");
        cJSON *expires = cJSON_GetObjectItem(json, "cert_expires_at");

        const char *cert_str = (cert_pem && cJSON_IsString(cert_pem)) ? cJSON_GetStringValue(cert_pem) : NULL;
        const char *key_str = (key_pem && cJSON_IsString(key_pem)) ? cJSON_GetStringValue(key_pem) : NULL;
        const char *ca_str = (ca_pem && cJSON_IsString(ca_pem)) ? cJSON_GetStringValue(ca_pem) : NULL;

        if (!cert_str || !key_str || !ca_str) {
            ESP_LOGE(TAG, "renew-cert response missing PEM field(s)");
        } else if (strlen(cert_str) >= MAKAPIX_PEM_MAX_LEN ||
                   strlen(key_str) >= MAKAPIX_PEM_MAX_LEN ||
                   strlen(ca_str) >= MAKAPIX_PEM_MAX_LEN) {
            // Same acquisition-time size gate as provisioning: NVS must never
            // hold an item the MQTT read paths cannot load.
            ESP_LOGE(TAG, "renew-cert PEM exceeds MAKAPIX_PEM_MAX_LEN (%zu/%zu/%zu)",
                     strlen(ca_str), strlen(cert_str), strlen(key_str));
        } else {
            result = makapix_store_save_renewed_certs(ca_str, cert_str, key_str);
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "Certificate renewed; server reports expiry %s",
                         (expires && cJSON_IsString(expires)) ? cJSON_GetStringValue(expires)
                                                              : "(not provided)");
            }
        }
        cJSON_Delete(json);
    } else {
        ESP_LOGE(TAG, "Failed to parse renew-cert response");
    }

    free(response_buf);
    return result;
}

// ---------------------------------------------------------------------------
// Renewal attempt (window check + token + renew + adopt)
// ---------------------------------------------------------------------------

/**
 * Local window check. Returns true when a renewal should be attempted now.
 * Jitter smears the fleet across ~RENEWAL_JITTER_RANGE_S at the window edge;
 * within RENEWAL_NO_JITTER_MARGIN_S of expiry (or past it) it always fires.
 */
static bool renewal_due_locally(time_t not_after, time_t now)
{
    if (now >= not_after) {
        return true;  // already expired
    }
    if (now >= not_after - RENEWAL_NO_JITTER_MARGIN_S) {
        return true;  // close to expiry: no jitter
    }
    time_t window_start = not_after - (time_t)CONFIG_MAKAPIX_CERT_RENEW_WINDOW_DAYS * 86400;
    if (now < window_start) {
        return false;
    }
    time_t jitter = (time_t)(esp_random() % RENEWAL_JITTER_RANGE_S);
    return now >= window_start + jitter;
}

esp_err_t makapix_renewal_attempt(bool force)
{
    if (!s_renewal_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!makapix_store_has_player_key() || !makapix_store_has_certificates()) {
        return ESP_ERR_INVALID_STATE;  // unregistered: nothing to renew
    }
    if (!clock_is_plausible()) {
        // A renewal decision (and the server's TLS handshake) needs a real
        // clock. Never talk to the API on a 1970 clock.
        ESP_LOGW(TAG, "Skipping renewal: system clock not set (SNTP pending)");
        if (xSemaphoreTake(s_renewal_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            status_set_result("no_clock");
            xSemaphoreGive(s_renewal_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_renewal_mutex, pdMS_TO_TICKS(60000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    time_t now = time(NULL);
    s_status.last_attempt = now;

    // Refresh the cached notAfter (also feeds /api/status).
    time_t not_after = 0;
    esp_err_t err = read_cert_not_after(&not_after);
    if (err == ESP_OK) {
        s_status.cert_parsed = true;
        s_status.not_after = not_after;
    } else {
        s_status.cert_parsed = false;
        ESP_LOGW(TAG, "Could not read stored cert expiry: %s", esp_err_to_name(err));
        // An unparseable cert can't pass a local window check; only the
        // forced (self-heal) path proceeds and lets the server decide.
        if (!force) {
            status_set_result("error");
            xSemaphoreGive(s_renewal_mutex);
            return ESP_FAIL;
        }
    }

    if (!force && !renewal_due_locally(not_after, now)) {
        status_set_result("not_due");
        xSemaphoreGive(s_renewal_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status.cert_parsed) {
        long days_left = (long)((not_after - now) / 86400);
        ESP_LOGI(TAG, "Attempting certificate renewal (%s; cert %s, %ld day(s) %s expiry)",
                 force ? "forced" : "window",
                 now >= not_after ? "EXPIRED" : "valid",
                 days_left < 0 ? -days_left : days_left,
                 now >= not_after ? "past" : "until");
    } else {
        ESP_LOGI(TAG, "Attempting certificate renewal (forced; stored cert unreadable)");
    }

    char player_key[37];
    if (makapix_store_get_player_key(player_key, sizeof(player_key)) != ESP_OK) {
        status_set_result("error");
        xSemaphoreGive(s_renewal_mutex);
        return ESP_FAIL;
    }

    char token[MAKAPIX_API_TOKEN_MAX_LEN];
    err = obtain_api_token(player_key, false, token, sizeof(token));
    if (err != ESP_OK) {
        status_set_result(err == ESP_ERR_NOT_FOUND ? "player_gone" : "error");
        xSemaphoreGive(s_renewal_mutex);
        return (err == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    int http_status = 0;
    err = call_renew_cert(token, &http_status);

    if (err == ESP_ERR_NOT_ALLOWED) {
        // Stored token stale or revoked (e.g. owner rotated it via the web):
        // rotate once and retry once.
        ESP_LOGW(TAG, "renew-cert returned 401/403; rotating token and retrying once");
        err = obtain_api_token(player_key, true, token, sizeof(token));
        if (err == ESP_OK) {
            err = call_renew_cert(token, &http_status);
        }
    }

    esp_err_t ret;
    if (err == ESP_OK) {
        s_status.last_success = time(NULL);
        status_set_result("renewed");
        // Refresh the cached expiry from the newly stored cert.
        if (read_cert_not_after(&not_after) == ESP_OK) {
            s_status.cert_parsed = true;
            s_status.not_after = not_after;
            ESP_LOGI(TAG, "New certificate valid for %ld day(s)",
                     (long)((not_after - time(NULL)) / 86400));
        }
        ret = ESP_OK;
    } else if (http_status == 400) {
        // Server-side window guard: cert comfortably valid. Informational.
        ESP_LOGI(TAG, "Server declined renewal (400): certificate not close enough to expiry");
        status_set_result("not_due");
        ret = ESP_ERR_INVALID_STATE;
    } else if (err == ESP_ERR_NOT_FOUND) {
        status_set_result("player_gone");
        ret = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_ERR_NOT_ALLOWED) {
        ESP_LOGE(TAG, "renew-cert still unauthorized after a fresh token");
        status_set_result("auth_failed");
        ret = ESP_ERR_NOT_ALLOWED;
    } else if (err == ESP_ERR_INVALID_RESPONSE && http_status == 429) {
        ESP_LOGW(TAG, "renew-cert rate-limited (429); will retry at the next check");
        status_set_result("rate_limited");
        ret = err;
    } else {
        status_set_result("error");
        ret = err;
    }

    xSemaphoreGive(s_renewal_mutex);

    if (ret == ESP_OK) {
        // Adoption. The reconnect path loads certificates from NVS on every
        // cycle, so a disconnected device picks the new pair up on its next
        // attempt. A connected device is left alone (make-before-break: the
        // old cert stays valid until its own notAfter). Only a latched
        // registration needs an explicit restart of the machinery.
        if (s_makapix_state == MAKAPIX_STATE_REGISTRATION_INVALID) {
            ESP_LOGI(TAG, "Renewal succeeded while registration was latched invalid — clearing latch and reconnecting");
            makapix_mqtt_reset_auth_failure_count();
            makapix_set_state(MAKAPIX_STATE_DISCONNECTED);
            (void)makapix_ensure_reconnect_task(true);
        }
    }
    return ret;
}

bool makapix_renewal_cert_expired(void)
{
    if (!clock_is_plausible() || !makapix_store_has_certificates()) {
        return false;
    }
    time_t not_after = 0;
    if (read_cert_not_after(&not_after) != ESP_OK) {
        return false;
    }
    return time(NULL) >= not_after;
}

void makapix_renewal_get_status(makapix_renewal_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_renewal_mutex ||
        xSemaphoreTake(s_renewal_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    // Populate the cached expiry on first ask (e.g. UI opened before the
    // first periodic check ran) so the badge doesn't show "unknown" for
    // 90 seconds after boot.
    if (!s_status.cert_parsed && makapix_store_has_certificates()) {
        time_t not_after = 0;
        if (read_cert_not_after(&not_after) == ESP_OK) {
            s_status.cert_parsed = true;
            s_status.not_after = not_after;
        }
    }
    *out = s_status;
    xSemaphoreGive(s_renewal_mutex);
}

// ---------------------------------------------------------------------------
// Periodic task
// ---------------------------------------------------------------------------

/**
 * Block until the device can meaningfully evaluate a renewal: Wi-Fi holds an
 * IP address and the system clock has been SNTP-synchronized. Both are
 * re-checked before every attempt (Wi-Fi can drop between ticks; SNTP resets
 * on sntp_sync_stop). SNTP success also proves internet reachability — NTP
 * responses came back — so no separate probe is needed.
 */
static void wait_for_network_and_time(void)
{
    bool logged = false;
    while (true) {
        char wifi_ip[16];
        bool online = (app_wifi_get_local_ip(wifi_ip, sizeof(wifi_ip)) == ESP_OK &&
                       strcmp(wifi_ip, "0.0.0.0") != 0);
        if (online && sntp_sync_is_synchronized()) {
            return;
        }
        if (!logged) {
            ESP_LOGI(TAG, "Waiting for %s before certificate check",
                     online ? "SNTP time sync" : "network connectivity");
            logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(RENEWAL_PREREQ_POLL_MS));
    }
}

static void renewal_task(void *pvParameters)
{
    (void)pvParameters;

    const TickType_t check_interval =
        pdMS_TO_TICKS((uint64_t)CONFIG_MAKAPIX_CERT_RENEW_CHECK_HOURS * 3600 * 1000);

    while (true) {
        // Gate every check on the prerequisites (covers the expired-cert
        // cold start: MQTT can't connect, so nothing else would trigger us,
        // and the check must not fire before the clock is real).
        wait_for_network_and_time();

        esp_err_t err = makapix_renewal_attempt(false);
        // ESP_OK and not-due are the normal outcomes; everything else was
        // already logged with its cause and simply waits for the next tick.
        (void)err;

        // Sleep until the next periodic check, or an early kick
        // (MQTT connect). Multiple kicks coalesce.
        ulTaskNotifyTake(pdTRUE, check_interval);
    }
}

void makapix_renewal_kick(void)
{
    if (s_renewal_task_handle) {
        xTaskNotifyGive(s_renewal_task_handle);
    }
}

esp_err_t makapix_renewal_init(void)
{
    if (s_renewal_task_handle) {
        return ESP_OK;
    }

    if (!s_renewal_mutex) {
        s_renewal_mutex = xSemaphoreCreateMutex();
        if (!s_renewal_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_renewal_stack) {
        s_renewal_stack = heap_caps_malloc(RENEWAL_TASK_STACK_SIZE * sizeof(StackType_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    BaseType_t ret = pdFAIL;
    if (s_renewal_stack) {
        s_renewal_task_handle = xTaskCreateStatic(renewal_task, "cert_renew",
                                                  RENEWAL_TASK_STACK_SIZE, NULL,
                                                  CONFIG_P3A_NETWORK_TASK_PRIORITY,
                                                  s_renewal_stack, &s_renewal_task_buffer);
        ret = (s_renewal_task_handle != NULL) ? pdPASS : pdFAIL;
    }
    if (ret != pdPASS) {
        ret = xTaskCreate(renewal_task, "cert_renew", RENEWAL_TASK_STACK_SIZE, NULL,
                          CONFIG_P3A_NETWORK_TASK_PRIORITY, &s_renewal_task_handle);
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create certificate renewal task");
        s_renewal_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Certificate renewal task started (window %d days, check every %d h)",
             CONFIG_MAKAPIX_CERT_RENEW_WINDOW_DAYS, CONFIG_MAKAPIX_CERT_RENEW_CHECK_HOURS);
    return ESP_OK;
}
