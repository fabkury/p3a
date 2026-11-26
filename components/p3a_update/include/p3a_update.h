// SPDX-License-Identifier: Apache-2.0
// Firmware updates from GitHub Releases.
//
// The device looks at the repository's releases (two minutes after it gets
// a network, then every twelve hours, and when asked), and says when one
// has a newer image for it. Nothing is installed until the owner asks: the
// install is a job that writes the image to the other app slot, checks it
// against the SHA-256 GitHub holds, and restarts into it.
//
// A newly installed firmware is on trial. It is kept once the owner can
// reach it again (the network is joined and the web server is up); a
// firmware that has not got that far ten minutes after it started is given
// up and the device goes back to the one before (decision D86).
//
// The other slot keeps the firmware that ran before, and the owner can go
// back to it at any time.
//
// What is decided without hardware is update_release.h (which release) and
// update_policy.h (when to look, the trial).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3a_job.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_UPDATE_IDLE = 0,
    P3A_UPDATE_CHECKING,
    P3A_UPDATE_INSTALLING,
    P3A_UPDATE_RESTARTING,  // installed, or going back: the restart follows within seconds
} p3a_update_state_t;

typedef struct {
    p3a_update_state_t state;
    uint8_t progress;           // 0 to 100 while installing
    char running[32];           // this firmware's version
    bool on_trial;              // installed and not yet kept
    char other[32];             // the firmware in the other slot that can be gone back to; "" if none
    char undone[32];            // a firmware that was installed, did not prove itself and was left; "" if none

    bool checked;               // a look was attempted since boot
    uint32_t checked_ago_s;     // seconds since it ended
    esp_err_t check_error;      // ESP_OK if it got an answer

    bool has_latest;            // the newest release this device could install is known
    bool available;             // and it is newer than what runs
    char latest[32];
    bool latest_prerelease;
    uint32_t latest_bytes;
    char latest_published[24];
    char latest_notes[1024];

    esp_err_t install_error;    // why the last install since boot failed; ESP_OK if none did
    char install_error_text[96];
    const char *blocked;        // why an install cannot start now ("CARD_IN_USE"), or NULL
} p3a_update_status_t;

// Reads which firmware runs, whether it is on trial and what the other slot
// holds. Call from app_main's task, before anything else here: it maps flash.
void p3a_update_boot(void);

// Adds the HTTP routes and starts the clock that looks for releases and
// judges the trial. Call after p3a_net_start().
esp_err_t p3a_update_start(void);

// app_main says the boot sequence ran to its end.
void p3a_update_boot_done(void);

// The struct is 1.3 KB: keep it off small stacks.
void p3a_update_status(p3a_update_status_t *out);

// Each starts a job. ESP_ERR_INVALID_STATE when a check or an install is
// already running.
esp_err_t p3a_update_check(p3a_job_id_t *out_job);
// ESP_ERR_NOT_FOUND when no newer release is known; P3A_ERR_SD_FAILED when
// the card is with a computer (every such session ends in a restart, which
// would cut the install).
esp_err_t p3a_update_install(p3a_job_id_t *out_job);
// Back to the firmware in the other slot, then a restart. ESP_ERR_NOT_FOUND
// when there is none.
esp_err_t p3a_update_roll_back(p3a_job_id_t *out_job);

// Where releases are listed and where their images must be. For test rigs,
// which serve a mirror on the LAN; nothing in the firmware proper calls it
// and no setting or API reaches it. NULL restores GitHub.
void p3a_update_set_source(const char *list_url, const char *download_prefix);

#ifdef __cplusplus
}
#endif
