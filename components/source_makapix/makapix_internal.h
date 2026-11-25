// SPDX-License-Identifier: Apache-2.0
// What the Makapix source's files share. Private to the component.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "p3a_source.h"
#include "source_makapix.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- makapix_register.c: the one long-lived task, and registering ---
esp_err_t makapix_task_start(void);
// Fills the registration's part of the status: phase, code, time left, last result.
void makapix_register_status(source_makapix_status_t *out);

// --- makapix_link.c: the MQTT link. The tick runs on the Makapix task ---
esp_err_t makapix_link_start(void);
void makapix_link_tick(void);
void makapix_link_shown(const p3a_record_t *record, const p3a_shown_t *how);
void makapix_link_status(source_makapix_status_t *out);  // needs out->registered set
void makapix_link_certificates_changed(void);
bool makapix_link_is_invalid(void);

// --- makapix_renew.c: the certificates. On the Makapix task ---
esp_err_t makapix_renew_start(void);
void makapix_renew_tick(bool link_invalid);
void makapix_renew_kick(void);  // look again at the next tick
// Asks for new certificates whatever the dates. True when they were saved;
// *counted false when nothing could be asked (no clock, no memory).
bool makapix_renew_now(bool *counted);
void makapix_renew_status(source_makapix_status_t *out);

#ifdef __cplusplus
}
#endif
