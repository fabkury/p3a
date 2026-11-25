// SPDX-License-Identifier: Apache-2.0
// Renewing the player's certificates. When one is due is
// makapix_link_logic.c (pure); this file reads the stored certificate's
// end, asks the server, and saves what comes back.
//
//   POST https://makapix.club/api/player/renew-cert    Authorization: Bearer {token}
//   200: {"cert_pem", "key_pem", "ca_pem", "cert_expires_at"}
//   400: the server's own window is not open yet
//   401: the token (replaced once and tried again, by makapix_authorized_post)
//   404: the server has no such player
//
// The old certificate stays good until its own end, so a connection that
// is up stays up; the new one is used from the next connection on.
//
// Runs on the Makapix task: once a day, at every connection made, and when
// the link asks for it because the broker and the player do not accept
// each other (makapix_renew_now).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "makapix_internal.h"
#include "makapix_link_logic.h"
#include "makapix_parse.h"
#include "makapix_rpc.h"
#include "makapix_store.h"
#include "mbedtls/x509_crt.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_mem.h"
#include "psram_alloc.h"

static const char *TAG = "makapix";

#define RENEW_URL       "https://" MAKAPIX_HOST "/api/player/renew-cert"
#define CHECK_PERIOD_MS (24LL * 3600 * 1000)
#define ANSWER_BYTES    (32 * 1024)

static SemaphoreHandle_t s_lock;
static int64_t s_next_check_ms;
// Guarded by s_lock.
static uint32_t s_not_after;
static char s_result[SOURCE_MAKAPIX_RENEWAL_LEN];

static void set_result(const char *result)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_result, sizeof(s_result), "%s", result);
    xSemaphoreGive(s_lock);
}

// When the stored certificate ends, Unix seconds; 0 if it cannot be read.
static uint32_t stored_not_after(void)
{
    makapix_pems_t pems = {0};
    if (makapix_store_pems(&pems) != ESP_OK) {
        return 0;
    }
    uint32_t ends = 0;
    mbedtls_x509_crt *crt = psram_calloc(1, sizeof(*crt));
    if (crt != NULL) {
        mbedtls_x509_crt_init(crt);
        if (mbedtls_x509_crt_parse(crt, (const unsigned char *)pems.cert, strlen(pems.cert) + 1) == 0) {
            char text[32];
            snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", crt->valid_to.year % 10000,
                     crt->valid_to.mon % 100, crt->valid_to.day % 100, crt->valid_to.hour % 100, crt->valid_to.min % 100,
                     crt->valid_to.sec % 100);
            ends = makapix_time(text);
        }
        mbedtls_x509_crt_free(crt);
        free(crt);
    }
    makapix_pems_free(&pems);
    return ends;
}

// One attempt. Returns true when new certificates were saved. `*counted`
// (may be NULL) is false when nothing could even be asked.
static bool attempt(bool forced, bool *counted)
{
    if (counted != NULL) {
        *counted = false;
    }
    if (!makapix_store_registered()) {
        return false;
    }
    const uint32_t now = p3a_clock_is_set() ? (uint32_t)time(NULL) : 0;
    const uint32_t ends = stored_not_after();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_not_after = ends;
    xSemaphoreGive(s_lock);
    const makapix_renew_due_t due = makapix_renew_due(now, ends, esp_random(), forced);
    if (due == MAKAPIX_RENEW_NO_CLOCK) {
        set_result("no_clock");
        return false;
    }
    if (due == MAKAPIX_RENEW_NOT_DUE) {
        set_result("not_due");
        return false;
    }
    if (p3a_mem_level() != P3A_MEM_OK) {
        return false;  // not now; asked again at the next check
    }
    if (counted != NULL) {
        *counted = true;
    }
    char *answer = psram_malloc(ANSWER_BYTES);
    char *pems = psram_malloc(3 * MAKAPIX_PEM_MAX);
    makapix_credentials_t *got = psram_calloc(1, sizeof(*got));
    esp_err_t err = answer != NULL && pems != NULL && got != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = makapix_authorized_post(RENEW_URL, NULL, answer, ANSWER_BYTES, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = makapix_parse_credentials(answer, pems, 3 * MAKAPIX_PEM_MAX, got);
    }
    if (answer != NULL) {
        memset(answer, 0, ANSWER_BYTES);  // it held the private key
    }
    if (err == ESP_OK) {
        const makapix_pems_t borrowed = {.ca = (char *)got->ca, .cert = (char *)got->cert, .key = (char *)got->key};
        err = makapix_store_save_pems(&borrowed);
    }
    if (pems != NULL) {
        memset(pems, 0, 3 * MAKAPIX_PEM_MAX);
    }
    free(got);
    free(pems);
    free(answer);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "the player's certificates were renewed");
        set_result("renewed");
        const uint32_t renewed_ends = stored_not_after();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_not_after = renewed_ends;
        xSemaphoreGive(s_lock);
        makapix_link_certificates_changed();
        return true;
    }
    // What the refusal was. http_fetch tells 401 and 403, 404 and 429 apart;
    // a 400 (the server's window is not open) is among "everything else".
    const char *result = err == (esp_err_t)P3A_ERR_AUTH           ? "auth_failed"
                         : err == (esp_err_t)P3A_ERR_MISSING      ? "player_gone"
                         : err == (esp_err_t)P3A_ERR_RATE_LIMITED ? "rate_limited"
                                                                  : "error";
    ESP_LOGW(TAG, "the certificates were not renewed: %s (%s)", result, p3a_err_to_name(err));
    set_result(result);
    return false;
}

bool makapix_renew_now(bool *counted)
{
    return attempt(true, counted);
}

void makapix_renew_tick(bool link_invalid)
{
    const int64_t now = esp_timer_get_time() / 1000;
    if (now < s_next_check_ms || !makapix_store_registered()) {
        return;
    }
    // Without a clock nothing can be decided: looked at again in a minute.
    if (!p3a_clock_is_set()) {
        s_next_check_ms = now + 60000;
        return;
    }
    s_next_check_ms = now + CHECK_PERIOD_MS;
    // A registration the broker no longer accepts is given a renewal at every check.
    (void)attempt(link_invalid, NULL);
}

void makapix_renew_kick(void)
{
    s_next_check_ms = 0;
}

void makapix_renew_status(source_makapix_status_t *out)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->cert_not_after = s_not_after;
    memcpy(out->renewal, s_result, sizeof(out->renewal));
    xSemaphoreGive(s_lock);
}

esp_err_t makapix_renew_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}
