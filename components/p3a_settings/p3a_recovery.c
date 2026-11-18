// SPDX-License-Identifier: Apache-2.0
// Recovery counters on NVS. See p3a_recovery.h.

#include "p3a_recovery.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "recovery";

#define NVS_NAMESPACE "p3a_boot"

static const char *const s_names[P3A_RECOVERY_KIND_COUNT_] = {"touch", "wifi", "link"};

const char *p3a_recovery_name(p3a_recovery_kind_t kind)
{
    return (int)kind >= 0 && kind < P3A_RECOVERY_KIND_COUNT_ ? s_names[kind] : "?";
}

static void key_for(char *out, size_t out_len, p3a_recovery_kind_t kind, const char *which)
{
    snprintf(out, out_len, "%s_%s", p3a_recovery_name(kind), which);
}

static bool read_counters(nvs_handle_t nvs, p3a_recovery_kind_t kind, uint16_t *total, uint16_t *streak)
{
    char key[16];
    *total = 0;
    *streak = 0;
    key_for(key, sizeof(key), kind, "tot");
    esp_err_t a = nvs_get_u16(nvs, key, total);
    key_for(key, sizeof(key), kind, "str");
    esp_err_t b = nvs_get_u16(nvs, key, streak);
    return (a == ESP_OK || a == ESP_ERR_NVS_NOT_FOUND) && (b == ESP_OK || b == ESP_ERR_NVS_NOT_FOUND);
}

static bool write_counter(nvs_handle_t nvs, p3a_recovery_kind_t kind, const char *which, uint16_t value)
{
    char key[16];
    key_for(key, sizeof(key), kind, which);
    return nvs_set_u16(nvs, key, value) == ESP_OK;
}

static uint16_t read_one(p3a_recovery_kind_t kind, bool want_total)
{
    uint16_t total = 0, streak = 0;
    nvs_handle_t nvs;
    if ((int)kind < 0 || kind >= P3A_RECOVERY_KIND_COUNT_ || nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return 0;  // nothing stored yet reads as zero
    }
    read_counters(nvs, kind, &total, &streak);
    nvs_close(nvs);
    return want_total ? total : streak;
}

uint16_t p3a_recovery_total(p3a_recovery_kind_t kind)
{
    return read_one(kind, true);
}

uint16_t p3a_recovery_streak(p3a_recovery_kind_t kind)
{
    return read_one(kind, false);
}

p3a_recovery_action_t p3a_recovery_report(p3a_recovery_kind_t kind, bool ok, uint16_t max_reboots)
{
    // Without working counters a restart loop could not be bounded.
    const p3a_recovery_action_t blind = ok ? P3A_RECOVERY_CONTINUE : P3A_RECOVERY_DEGRADED;
    if ((int)kind < 0 || kind >= P3A_RECOVERY_KIND_COUNT_) {
        return blind;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        if (!ok) {
            ESP_LOGE(TAG, "%s failed and the counters are unavailable; carrying on without it", s_names[kind]);
        }
        return blind;
    }
    uint16_t total, streak;
    p3a_recovery_action_t action = blind;
    if (read_counters(nvs, kind, &total, &streak)) {
        action = p3a_recovery_policy(streak, ok, max_reboots);
        bool stored = true;
        if (action == P3A_RECOVERY_CONTINUE) {
            if (streak > 0) {
                ESP_LOGI(TAG, "%s recovered after %u restart(s)", s_names[kind], (unsigned)streak);
                stored = write_counter(nvs, kind, "str", 0);
            }
        } else if (action == P3A_RECOVERY_REBOOT) {
            stored = write_counter(nvs, kind, "str", (uint16_t)(streak + 1)) &&
                     write_counter(nvs, kind, "tot", total < UINT16_MAX ? (uint16_t)(total + 1) : total);
        } else {
            ESP_LOGW(TAG, "%s still failing after %u restart(s); carrying on without it", s_names[kind],
                     (unsigned)streak);
        }
        stored = stored && nvs_commit(nvs) == ESP_OK;
        if (!stored && action == P3A_RECOVERY_REBOOT) {
            ESP_LOGE(TAG, "%s failed and the restart could not be counted; carrying on without it", s_names[kind]);
            action = P3A_RECOVERY_DEGRADED;
        }
    }
    nvs_close(nvs);
    return action;
}
