// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file art_institution_rate_limit.c
 * @brief Per-museum cooldown table shared by refresh, download, and the
 *        browser-side report-429 endpoint.
 *
 * Modeled on giphy_set_rate_limited / _is_rate_limited / _remaining_sec
 * in components/giphy/giphy_api.c, generalized to a fixed-size table
 * keyed by museum enum. State lives in RAM; reboot clears it.
 *
 * The table is sized by ART_INSTITUTION_NUM_MUSEUMS (a compile-time
 * constant from art_institution_types.h), so reads and writes are O(1)
 * and the structure scales to whichever museums M2+ add without code
 * changes here.
 */

#include "art_institution.h"
#include "art_institution_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "ai_ratelim";

// One slot per museum; index is museum_id_t. Stored as absolute
// esp_timer microseconds so reads never need locking.
static int64_t s_cooldown_until_us[ART_INSTITUTION_NUM_MUSEUMS];

void art_institution_rate_limit_reset(void)
{
    memset(s_cooldown_until_us, 0, sizeof(s_cooldown_until_us));
}

void art_institution_rate_limit_set_enum(museum_id_t id, uint32_t cooldown_sec)
{
    if ((size_t)id >= ART_INSTITUTION_NUM_MUSEUMS) return;
    if (cooldown_sec == 0) cooldown_sec = ART_INSTITUTION_DEFAULT_COOLDOWN_SEC;

    int64_t until = esp_timer_get_time() + (int64_t)cooldown_sec * 1000000LL;
    // Only extend; a longer existing cooldown wins so partial overlapping
    // 429 reports don't shorten the deadline.
    if (until > s_cooldown_until_us[id]) {
        s_cooldown_until_us[id] = until;
        ESP_LOGW(TAG, "Cooldown engaged for museum #%d: %lus",
                 (int)id, (unsigned long)cooldown_sec);
    }
}

bool art_institution_rate_limit_is_active_enum(museum_id_t id)
{
    if ((size_t)id >= ART_INSTITUTION_NUM_MUSEUMS) return false;
    return esp_timer_get_time() < s_cooldown_until_us[id];
}

uint32_t art_institution_rate_limit_remaining_enum(museum_id_t id)
{
    if ((size_t)id >= ART_INSTITUTION_NUM_MUSEUMS) return 0;
    int64_t now = esp_timer_get_time();
    int64_t until = s_cooldown_until_us[id];
    if (now >= until) return 0;
    return (uint32_t)((until - now + 999999) / 1000000);
}

// ----- Public string-keyed API --------------------------------------------

void art_institution_set_rate_limited(const char *museum_id, uint32_t cooldown_sec)
{
    const art_institution_museum_t *m = art_institution_find(museum_id);
    if (!m) return;
    art_institution_rate_limit_set_enum(m->museum_enum, cooldown_sec);
}

bool art_institution_is_rate_limited(const char *museum_id)
{
    const art_institution_museum_t *m = art_institution_find(museum_id);
    if (!m) return false;
    return art_institution_rate_limit_is_active_enum(m->museum_enum);
}

uint32_t art_institution_rate_limit_remaining(const char *museum_id)
{
    const art_institution_museum_t *m = art_institution_find(museum_id);
    if (!m) return 0;
    return art_institution_rate_limit_remaining_enum(m->museum_enum);
}
