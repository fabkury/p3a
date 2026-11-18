// SPDX-License-Identifier: Apache-2.0
// The settings store on NVS. See p3a_settings.h.

#include "p3a_settings.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "psram_alloc.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "p3a"
#define NVS_KEY       "settings"
#define NVS_KEY_BAD   "settings_bad"
#define MAX_BLOB      (16 * 1024)

static SemaphoreHandle_t s_lock;
static p3a_settings_t s_now;
static p3a_settings_t s_next;  // scratch for a change in progress; guarded by the lock
static bool s_have_defaults;
static bool s_can_save;
static bool s_reboot_needed;
static p3a_settings_stats_t s_stats;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void ensure_defaults(void)
{
    if (!s_have_defaults) {
        p3a_settings_defaults(&s_now);
        s_have_defaults = true;
    }
}

// Reads the stored blob into s_now. Called once, before anything else runs.
static void load(nvs_handle_t nvs)
{
    size_t len = 0;
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "nothing stored; using defaults");
        return;
    }
    if (err != ESP_OK || len == 0 || len > MAX_BLOB) {
        ESP_LOGE(TAG, "stored settings unreadable (%s, %u bytes); using defaults", esp_err_to_name(err),
                 (unsigned)len);
        s_stats.blob_unreadable = true;
        return;
    }
    char *text = psram_malloc(len);
    if (text == NULL) {
        ESP_LOGE(TAG, "no memory to read stored settings; using defaults");
        return;
    }
    err = nvs_get_blob(nvs, NVS_KEY, text, &len);
    cJSON *object = err == ESP_OK ? cJSON_ParseWithLength(text, len) : NULL;
    if (!cJSON_IsObject(object)) {
        // Keep what was there: it is the only copy of the user's settings.
        ESP_LOGE(TAG, "stored settings do not parse; kept aside as '%s', using defaults", NVS_KEY_BAD);
        s_stats.blob_unreadable = true;
        if (err == ESP_OK && nvs_set_blob(nvs, NVS_KEY_BAD, text, len) == ESP_OK) {
            nvs_commit(nvs);
        }
    } else {
        s_stats.skipped = p3a_settings_load_json(&s_now, object);
        s_stats.loaded = true;
        s_stats.stored_bytes = len;
        if (s_stats.skipped > 0) {
            ESP_LOGW(TAG, "%d stored entries ignored (unknown or no longer valid)", s_stats.skipped);
        }
        ESP_LOGI(TAG, "loaded (%u bytes stored)", (unsigned)len);
    }
    cJSON_Delete(object);
    free(text);
}

esp_err_t p3a_settings_start(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_defaults();
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition cannot be used as it is. Erasing loses the settings,
        // the Wi-Fi credentials and the Makapix registration, and is the only
        // way to a device that works.
        ESP_LOGE(TAG, "NVS unusable (%s): ERASING it. Settings and credentials are lost.", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS did not start: %s. Settings will not be saved.", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open the settings namespace: %s", esp_err_to_name(err));
        return err;
    }
    load(nvs);
    nvs_close(nvs);
    s_can_save = true;
    return ESP_OK;
}

void p3a_settings_get(p3a_settings_t *out)
{
    lock();
    ensure_defaults();
    *out = s_now;
    unlock();
}

// Writes every non-default value of *s as one blob. Called with the lock held.
static esp_err_t save(const p3a_settings_t *s)
{
    cJSON *object = p3a_settings_to_json(s, P3A_SETTINGS_JSON_ONLY_CHANGED);
    char *text = object != NULL ? cJSON_PrintUnformatted(object) : NULL;
    cJSON_Delete(object);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = strlen(text);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY, text, len);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    cJSON_free(text);
    if (err == ESP_OK) {
        s_stats.stored_bytes = len;
    }
    return err;
}

esp_err_t p3a_settings_set_json(const cJSON *object, uint64_t *changed, char *why, size_t why_len)
{
    uint64_t mask = 0;
    if (changed != NULL) {
        *changed = 0;
    }
    if (why != NULL && why_len > 0) {
        why[0] = '\0';
    }

    lock();
    ensure_defaults();
    if (!s_can_save) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_next = s_now;
    esp_err_t err = p3a_settings_apply_json(&s_next, object, &mask, why, why_len);
    if (err == ESP_OK && mask != 0) {
        err = save(&s_next);
        if (err == ESP_OK) {
            s_now = s_next;
            for (int i = 0; i < P3A_SET_COUNT; i++) {
                if ((mask & (1ULL << i)) && (p3a_setting_defs[i].flags & P3A_SET_FLAG_REBOOT)) {
                    s_reboot_needed = true;
                }
            }
        } else {
            ESP_LOGE(TAG, "could not save: %s", esp_err_to_name(err));
        }
    }
    unlock();
    if (err != ESP_OK) {
        return err;
    }

    // Outside the lock: a handler may read the settings.
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        if (mask & (1ULL << i)) {
            // Never the value: it may be a secret.
            ESP_LOGI(TAG, "%s changed", p3a_setting_defs[i].key);
            p3a_event_emit_i32(P3A_EVENT_SETTING_CHANGED, i);
        }
    }
    if (changed != NULL) {
        *changed = mask;
    }
    return ESP_OK;
}

static esp_err_t set_item(p3a_setting_id_t id, cJSON *value)
{
    if ((int)id < 0 || id >= P3A_SET_COUNT) {
        cJSON_Delete(value);
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *object = cJSON_CreateObject();
    if (object == NULL || value == NULL) {
        cJSON_Delete(object);
        cJSON_Delete(value);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(object, p3a_setting_defs[id].key, value);
    esp_err_t err = p3a_settings_set_json(object, NULL, NULL, 0);
    cJSON_Delete(object);
    return err;
}

esp_err_t p3a_settings_set_bool(p3a_setting_id_t id, bool value)
{
    return set_item(id, cJSON_CreateBool(value));
}

esp_err_t p3a_settings_set_int(p3a_setting_id_t id, int32_t value)
{
    return set_item(id, cJSON_CreateNumber((double)value));
}

esp_err_t p3a_settings_set_str(p3a_setting_id_t id, const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_item(id, cJSON_CreateString(value));
}

cJSON *p3a_settings_json(void)
{
    p3a_settings_t copy;
    p3a_settings_get(&copy);
    return p3a_settings_to_json(&copy, P3A_SETTINGS_JSON_MASK_SECRETS);
}

const char *p3a_settings_schema(size_t *len)
{
    extern const char schema_start[] asm("_binary_settings_json_start");
    extern const char schema_end[] asm("_binary_settings_json_end");
    if (len != NULL) {
        *len = (size_t)(schema_end - schema_start) - 1;  // EMBED_TXTFILES adds a terminator
    }
    return schema_start;
}

bool p3a_settings_reboot_needed(void)
{
    lock();
    bool needed = s_reboot_needed;
    unlock();
    return needed;
}

esp_err_t p3a_settings_factory_reset(void)
{
    lock();
    s_can_save = false;
    ESP_LOGW(TAG, "factory reset: erasing NVS");
    esp_err_t err = nvs_flash_erase();  // de-initialises NVS first
    unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
    }
    return err;
}

p3a_settings_stats_t p3a_settings_stats(void)
{
    lock();
    p3a_settings_stats_t stats = s_stats;
    unlock();
    return stats;
}
