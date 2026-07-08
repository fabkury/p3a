// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_renewal.h
 * @brief Device-initiated MQTT client-certificate renewal
 *
 * Implements the firmware half of the Makapix cert-renewal design
 * (docs/makapix-cert-renewal/PLAN.md; server side per the MPX repo's
 * docs/player/cert-renewal-plan.md):
 *
 *  - A periodic task reads the stored client certificate's notAfter locally
 *    and, once inside the renewal window (with jitter), calls
 *    POST /api/player/renew-cert with the device bearer token.
 *  - Devices with no stored token (the entire pre-renewal fleet) bootstrap
 *    one via POST /api/player/{player_key}/token/rotate, which is gated only
 *    on knowledge of the player_key.
 *  - Renewal is make-before-break: the server does not revoke the previous
 *    certificate, and the firmware persists the new cert/key/CA in a single
 *    NVS commit before anything starts using them.
 *  - The token works even after the certificate has expired, so a device
 *    that slept through its expiry self-heals instead of latching
 *    REGISTRATION_INVALID.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Snapshot of renewal state for status/UI surfaces
 */
typedef struct {
    bool   cert_parsed;        ///< not_after below is valid
    time_t not_after;          ///< client cert expiry (epoch, UTC)
    time_t last_attempt;       ///< 0 if no attempt since boot
    time_t last_success;       ///< 0 if no successful renewal since boot
    char   last_result[24];    ///< "renewed", "not_due", "auth_failed",
                               ///< "player_gone", "rate_limited", "error",
                               ///< "no_clock", "" (never ran)
} makapix_renewal_status_t;

/**
 * @brief Start the certificate-renewal background task
 *
 * Called once from makapix_init(). Safe to call when the device is not
 * registered — the task simply idles until credentials exist.
 *
 * @return ESP_OK on success (or if already started), error code otherwise
 */
esp_err_t makapix_renewal_init(void);

/**
 * @brief Ask the renewal task to run a check soon (non-blocking)
 *
 * Called on MQTT connect so a device that just came online evaluates its
 * certificate without waiting for the next periodic tick.
 */
void makapix_renewal_kick(void);

/**
 * @brief Run one renewal attempt synchronously
 *
 * Full sequence: bearer-token bootstrap (token/rotate) if no token is stored,
 * then renew-cert, then atomic NVS persist of the returned cert/key/CA. On a
 * 401 the token is rotated once and the renewal retried once.
 *
 * Serialized internally; concurrent callers block on the same mutex.
 *
 * @param force When false, returns ESP_ERR_INVALID_STATE without any network
 *              traffic unless the local clock says the cert is inside the
 *              renewal window. When true (self-heal path), the local window
 *              check is skipped and the server decides.
 *
 * @return ESP_OK             renewed and persisted
 *         ESP_ERR_INVALID_STATE  not due (local window check or server 400)
 *         ESP_ERR_NOT_FOUND  player unknown to the server (rotate returned
 *                            404) — registration is genuinely dead
 *         ESP_ERR_NOT_ALLOWED    still 401 after a fresh token — server-side
 *                            auth problem
 *         other              transient (network, 429, 5xx, NVS) — retry later
 */
esp_err_t makapix_renewal_attempt(bool force);

/**
 * @brief Get a snapshot of the renewal state (for /api/status)
 *
 * @param out Receives the snapshot (zeroed on entry)
 */
void makapix_renewal_get_status(makapix_renewal_status_t *out);

/**
 * @brief Whether the stored client certificate is expired per the local clock
 *
 * @return true only when the clock is plausible, the cert parses, and
 *         now >= notAfter. false in every doubtful case.
 */
bool makapix_renewal_cert_expired(void);

#ifdef __cplusplus
}
#endif
