// SPDX-License-Identifier: Apache-2.0
// The settings model against the real schema (components/p3a_settings/settings.json).
#include "p3a_recovery.h"
#include "p3a_settings_model.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

static p3a_settings_t s;
static char why[96];
static uint64_t changed;

void setUp(void)
{
    p3a_settings_defaults(&s);
    memset(why, 0, sizeof(why));
    changed = 0xDEAD;
}
void tearDown(void) {}

#define BIT(id) (1ULL << (id))

// Applies a JSON text to s.
static esp_err_t apply(const char *json)
{
    cJSON *object = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL_MESSAGE(object, json);
    esp_err_t err = p3a_settings_apply_json(&s, object, &changed, why, sizeof(why));
    cJSON_Delete(object);
    return err;
}

#define REFUSED(json)                                              \
    do {                                                           \
        p3a_settings_t before = s;                                 \
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, apply(json), json); \
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &s, sizeof(s), json);   \
        TEST_ASSERT_EQUAL_UINT64(0, changed);                      \
        TEST_ASSERT_TRUE_MESSAGE(why[0] != '\0', json);            \
    } while (0)

// --- the table ------------------------------------------------------------------

static int by_offset(const void *a, const void *b)
{
    const p3a_setting_def_t *x = *(const p3a_setting_def_t *const *)a;
    const p3a_setting_def_t *y = *(const p3a_setting_def_t *const *)b;
    return (int)x->offset - (int)y->offset;
}

static void test_fields_lie_inside_the_struct_and_do_not_overlap(void)
{
    const p3a_setting_def_t *sorted[P3A_SET_COUNT];
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        sorted[i] = &p3a_setting_defs[i];
        TEST_ASSERT_TRUE(p3a_setting_defs[i].size > 0);
        TEST_ASSERT_TRUE((size_t)p3a_setting_defs[i].offset + p3a_setting_defs[i].size <= sizeof(p3a_settings_t));
    }
    qsort(sorted, P3A_SET_COUNT, sizeof(sorted[0]), by_offset);
    for (int i = 1; i < P3A_SET_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(sorted[i - 1]->offset + sorted[i - 1]->size <= sorted[i]->offset, sorted[i]->key);
    }
}

static void test_every_key_is_found_and_ids_follow_the_table(void)
{
    TEST_ASSERT_TRUE(P3A_SET_COUNT <= 64);
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        TEST_ASSERT_EQUAL(i, p3a_settings_find(p3a_setting_defs[i].key));
    }
    TEST_ASSERT_EQUAL(P3A_SET_BRIGHTNESS, p3a_settings_find("brightness"));
    TEST_ASSERT_EQUAL(-1, p3a_settings_find("Brightness"));
    TEST_ASSERT_EQUAL(-1, p3a_settings_find(""));
    TEST_ASSERT_EQUAL(-1, p3a_settings_find(NULL));
}

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL(100, s.brightness);
    TEST_ASSERT_EQUAL(0, s.rotation);
    TEST_ASSERT_EQUAL_HEX32(0x000000, s.background_color);
    TEST_ASSERT_EQUAL(30, s.dwell_s);
    TEST_ASSERT_TRUE(s.indicator_enabled);
    TEST_ASSERT_FALSE(s.show_fps);
    TEST_ASSERT_EQUAL(P3A_GIPHY_RATING_PG_13, s.giphy_rating);
    TEST_ASSERT_EQUAL(P3A_CHANNEL_SELECT_STOCHASTIC, s.channel_select);
    TEST_ASSERT_EQUAL_STRING("fixed_height", s.giphy_rendition);
    TEST_ASSERT_EQUAL_STRING("", s.device_name);
    TEST_ASSERT_EQUAL_STRING("", s.giphy_api_key);
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(p3a_settings_is_default(&s, (p3a_setting_id_t)i), p3a_setting_defs[i].key);
    }
}

static void test_reboot_advanced_and_secret_flags_come_from_the_schema(void)
{
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_SD_ROOT].flags & P3A_SET_FLAG_REBOOT);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_DEVICE_NAME].flags & P3A_SET_FLAG_REBOOT);
    TEST_ASSERT_FALSE(p3a_setting_defs[P3A_SET_BRIGHTNESS].flags & P3A_SET_FLAG_REBOOT);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_GIPHY_RENDITION].flags & P3A_SET_FLAG_ADVANCED);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_MAKAPIX_VIEW_ACK].flags & P3A_SET_FLAG_ADVANCED);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_INDICATOR_ENABLED].flags & P3A_SET_FLAG_ADVANCED);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_GIPHY_API_KEY].flags & P3A_SET_FLAG_SECRET);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_KLIPY_API_KEY].flags & P3A_SET_FLAG_SECRET);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_HARVARD_API_KEY].flags & P3A_SET_FLAG_SECRET);
    TEST_ASSERT_TRUE(p3a_setting_defs[P3A_SET_SMITHSONIAN_API_KEY].flags & P3A_SET_FLAG_SECRET);
}

// --- applying -------------------------------------------------------------------

static void test_apply_one_of_each_type(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"brightness\":40,\"show_fps\":true,\"giphy_rating\":\"g\","
                                    "\"background_color\":\"#1A2b3C\",\"device_name\":\"living-room\",\"rotation\":270}"));
    TEST_ASSERT_EQUAL(40, s.brightness);
    TEST_ASSERT_TRUE(s.show_fps);
    TEST_ASSERT_EQUAL(P3A_GIPHY_RATING_G, s.giphy_rating);
    TEST_ASSERT_EQUAL_HEX32(0x1A2B3C, s.background_color);
    TEST_ASSERT_EQUAL_STRING("living-room", s.device_name);
    TEST_ASSERT_EQUAL(270, s.rotation);
    TEST_ASSERT_EQUAL_UINT64(BIT(P3A_SET_BRIGHTNESS) | BIT(P3A_SET_SHOW_FPS) | BIT(P3A_SET_GIPHY_RATING) |
                                 BIT(P3A_SET_BACKGROUND_COLOR) | BIT(P3A_SET_DEVICE_NAME) | BIT(P3A_SET_ROTATION),
                             changed);
    TEST_ASSERT_FALSE(p3a_settings_is_default(&s, P3A_SET_BRIGHTNESS));
    TEST_ASSERT_TRUE(p3a_settings_is_default(&s, P3A_SET_DWELL_S));
}

static void test_writing_the_value_already_held_changes_nothing(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"brightness\":100,\"giphy_rating\":\"pg-13\",\"device_name\":\"\"}"));
    TEST_ASSERT_EQUAL_UINT64(0, changed);
    TEST_ASSERT_EQUAL(ESP_OK, apply("{}"));
    TEST_ASSERT_EQUAL_UINT64(0, changed);
}

static void test_one_bad_entry_refuses_the_whole_request(void)
{
    p3a_settings_t before = s;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply("{\"brightness\":40,\"dwell_s\":60,\"rotation\":45}"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(why, "rotation"));
}

static void test_unknown_key_is_refused_and_named(void)
{
    p3a_settings_t before = s;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, apply("{\"brightness\":40,\"brightnes\":50}"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("brightnes is not a setting", why);
    // A 1.x name is not a 2.0 setting.
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, apply("{\"dwell_time_ms\":30000}"));
}

static void test_a_hostile_key_cannot_overflow_or_corrupt_the_message(void)
{
    char json[400];
    char key[300];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    key[3] = '\n';
    snprintf(json, sizeof(json), "{\"%.*s\":1}", 3, key);  // short sanity case
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, apply(json));

    cJSON *object = cJSON_CreateObject();
    cJSON_AddNumberToObject(object, key, 1);
    char small[24];
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, p3a_settings_apply_json(&s, object, NULL, small, sizeof(small)));
    TEST_ASSERT_EQUAL(sizeof(small) - 1, strlen(small));
    TEST_ASSERT_NULL(strchr(small, '\n'));
    // No message buffer at all is fine too.
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, p3a_settings_apply_json(&s, object, NULL, NULL, 0));
    cJSON_Delete(object);
}

static void test_not_an_object(void)
{
    REFUSED("[1,2]");
    REFUSED("\"brightness\"");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_settings_apply_json(&s, NULL, &changed, why, sizeof(why)));
}

static void test_bool_takes_only_true_or_false(void)
{
    REFUSED("{\"show_fps\":1}");
    REFUSED("{\"show_fps\":\"true\"}");
    REFUSED("{\"show_fps\":null}");
}

static void test_int_range_edges(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"brightness\":0}"));
    TEST_ASSERT_EQUAL(0, s.brightness);
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"brightness\":100}"));
    REFUSED("{\"brightness\":101}");
    REFUSED("{\"brightness\":-1}");
    REFUSED("{\"brightness\":50.5}");
    REFUSED("{\"brightness\":\"50\"}");
    REFUSED("{\"brightness\":true}");
    REFUSED("{\"brightness\":1e30}");
    REFUSED("{\"brightness\":-1e30}");
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"brightness\":5e1}"));
    TEST_ASSERT_EQUAL(50, s.brightness);
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"dwell_s\":0}"));
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"dwell_s\":86400}"));
    REFUSED("{\"dwell_s\":86401}");
}

static void test_int_with_choices(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"rotation\":90}"));
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"rotation\":180}"));
    REFUSED("{\"rotation\":91}");
    REFUSED("{\"rotation\":360}");
    REFUSED("{\"rotation\":-90}");
    TEST_ASSERT_EQUAL(180, s.rotation);
}

static void test_enum_takes_only_listed_names(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"pick_mode\":\"recency\",\"klipy_format\":\"webp\"}"));
    TEST_ASSERT_EQUAL(P3A_PICK_MODE_RECENCY, s.pick_mode);
    TEST_ASSERT_EQUAL(P3A_KLIPY_FORMAT_WEBP, s.klipy_format);
    REFUSED("{\"pick_mode\":\"Recency\"}");
    REFUSED("{\"pick_mode\":\"\"}");
    REFUSED("{\"pick_mode\":1}");
    REFUSED("{\"giphy_rating\":\"x\"}");
}

static void test_color(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"background_color\":\"#ffffff\"}"));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, s.background_color);
    REFUSED("{\"background_color\":\"ffffff\"}");
    REFUSED("{\"background_color\":\"#fff\"}");
    REFUSED("{\"background_color\":\"#gggggg\"}");
    REFUSED("{\"background_color\":\"#ffffff0\"}");
    REFUSED("{\"background_color\":16777215}");
}

static void test_string_length_and_characters(void)
{
    // device_name holds 16 characters.
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"device_name\":\"abcdefghijklmnop\"}"));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnop", s.device_name);
    REFUSED("{\"device_name\":\"abcdefghijklmnopq\"}");
    REFUSED("{\"giphy_rendition\":\"tab\\there\"}");
    REFUSED("{\"giphy_rendition\":\"caf\\u00e9\"}");
    REFUSED("{\"giphy_rendition\":7}");
    // A shorter value fully replaces a longer one.
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"device_name\":\"ab\"}"));
    TEST_ASSERT_EQUAL_STRING("ab", s.device_name);
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"device_name\":\"\"}"));
    TEST_ASSERT_TRUE(p3a_settings_is_default(&s, P3A_SET_DEVICE_NAME));
}

static void test_device_name_rule(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"device_name\":\"fab\"}"));
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"device_name\":\"a-1\"}"));
    REFUSED("{\"device_name\":\"-fab\"}");
    REFUSED("{\"device_name\":\"fab-\"}");
    REFUSED("{\"device_name\":\"Fab\"}");
    REFUSED("{\"device_name\":\"my room\"}");
    REFUSED("{\"device_name\":\"a.b\"}");
}

static void test_sd_root_rule(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"sd_root\":\"/art\"}"));
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"sd_root\":\"/sdcard/art/p3a\"}"));
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"sd_root\":\"\"}"));
    REFUSED("{\"sd_root\":\"art\"}");
    REFUSED("{\"sd_root\":\"/\"}");
    REFUSED("{\"sd_root\":\"/a/../b\"}");
    REFUSED("{\"sd_root\":\"/sdcard/\"}");
}

static void test_country_is_two_letters_stored_upper_case(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_country\":\"br\"}"));
    TEST_ASSERT_EQUAL_STRING("BR", s.giphy_country);
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_country\":\"Us\"}"));
    TEST_ASSERT_EQUAL_STRING("US", s.giphy_country);
    // Same value in another case is not a change.
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_country\":\"us\"}"));
    TEST_ASSERT_EQUAL_UINT64(0, changed);
    REFUSED("{\"giphy_country\":\"b\"}");
    REFUSED("{\"giphy_country\":\"b1\"}");
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_country\":\"\"}"));
    TEST_ASSERT_EQUAL_STRING("", s.giphy_country);
}

// --- secrets --------------------------------------------------------------------

static char *print(unsigned flags)
{
    cJSON *object = p3a_settings_to_json(&s, flags);
    TEST_ASSERT_NOT_NULL(object);
    char *text = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    TEST_ASSERT_NOT_NULL(text);
    return text;
}

static void test_secrets_are_masked_when_read_and_kept_when_the_mask_is_written_back(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_api_key\":\"s3cr3t-value\"}"));
    char *text = print(P3A_SETTINGS_JSON_MASK_SECRETS);
    TEST_ASSERT_NULL(strstr(text, "s3cr3t-value"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"giphy_api_key\":\"" P3A_SETTINGS_SECRET_MASK "\""));
    // An unset secret reads as empty, so a page can tell "set" from "not set".
    TEST_ASSERT_NOT_NULL(strstr(text, "\"klipy_api_key\":\"\""));

    // Posting the whole masked object back changes nothing and loses nothing.
    cJSON *object = cJSON_Parse(text);
    cJSON_free(text);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_settings_apply_json(&s, object, &changed, why, sizeof(why)));
    cJSON_Delete(object);
    TEST_ASSERT_EQUAL_UINT64(0, changed);
    TEST_ASSERT_EQUAL_STRING("s3cr3t-value", s.giphy_api_key);
    TEST_ASSERT_EQUAL_STRING("", s.klipy_api_key);

    // An empty string clears it.
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_api_key\":\"\"}"));
    TEST_ASSERT_EQUAL_STRING("", s.giphy_api_key);

    // The storage form is not masked.
    TEST_ASSERT_EQUAL(ESP_OK, apply("{\"giphy_api_key\":\"s3cr3t-value\"}"));
    text = print(P3A_SETTINGS_JSON_ONLY_CHANGED);
    TEST_ASSERT_EQUAL_STRING("{\"giphy_api_key\":\"s3cr3t-value\"}", text);
    cJSON_free(text);
}

// --- to JSON and back -----------------------------------------------------------

static void test_full_json_lists_every_setting(void)
{
    cJSON *object = p3a_settings_to_json(&s, 0);
    TEST_ASSERT_EQUAL(P3A_SET_COUNT, cJSON_GetArraySize(object));
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(cJSON_GetObjectItemCaseSensitive(object, p3a_setting_defs[i].key),
                                     p3a_setting_defs[i].key);
    }
    // The full form applies cleanly onto itself.
    TEST_ASSERT_EQUAL(ESP_OK, p3a_settings_apply_json(&s, object, &changed, why, sizeof(why)));
    TEST_ASSERT_EQUAL_UINT64(0, changed);
    cJSON_Delete(object);
}

static void test_defaults_store_as_an_empty_object(void)
{
    char *text = print(P3A_SETTINGS_JSON_ONLY_CHANGED);
    TEST_ASSERT_EQUAL_STRING("{}", text);
    cJSON_free(text);
}

// Moves every setting off its default, stores, reloads onto defaults, compares.
static void test_storage_round_trip_of_every_setting(void)
{
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        const p3a_setting_def_t *d = &p3a_setting_defs[i];
        cJSON *object = cJSON_CreateObject();
        switch (d->type) {
        case P3A_SET_TYPE_BOOL:
            cJSON_AddBoolToObject(object, d->key, d->default_int == 0);
            break;
        case P3A_SET_TYPE_INT:
            if (d->choice_count > 0) {
                cJSON_AddNumberToObject(object, d->key, d->choices[d->choices[0] == d->default_int ? 1 : 0]);
            } else {
                cJSON_AddNumberToObject(object, d->key, d->default_int == d->max ? d->min : d->max);
            }
            break;
        case P3A_SET_TYPE_ENUM:
            cJSON_AddStringToObject(object, d->key, d->values[d->default_int == 0 ? 1 : 0]);
            break;
        case P3A_SET_TYPE_COLOR:
            cJSON_AddStringToObject(object, d->key, "#0180ff");
            break;
        default:
            cJSON_AddStringToObject(object, d->key,
                                    d->check == P3A_SET_CHECK_SD_ROOT ? "/x" : d->check == P3A_SET_CHECK_COUNTRY ? "JP" : "x1");
            break;
        }
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, p3a_settings_apply_json(&s, object, &changed, why, sizeof(why)), why);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(BIT(i), changed, d->key);
        cJSON_Delete(object);
    }
    for (int i = 0; i < P3A_SET_COUNT; i++) {
        TEST_ASSERT_FALSE_MESSAGE(p3a_settings_is_default(&s, (p3a_setting_id_t)i), p3a_setting_defs[i].key);
    }

    char *text = print(P3A_SETTINGS_JSON_ONLY_CHANGED);
    cJSON *stored = cJSON_Parse(text);
    cJSON_free(text);
    TEST_ASSERT_EQUAL(P3A_SET_COUNT, cJSON_GetArraySize(stored));
    p3a_settings_t loaded;
    p3a_settings_defaults(&loaded);
    TEST_ASSERT_EQUAL(0, p3a_settings_load_json(&loaded, stored));
    cJSON_Delete(stored);
    TEST_ASSERT_EQUAL_MEMORY(&s, &loaded, sizeof(s));
}

static void test_load_skips_what_it_cannot_use_and_keeps_the_rest(void)
{
    cJSON *stored = cJSON_Parse("{\"brightness\":40,\"removed_setting\":1,\"rotation\":45,\"dwell_s\":\"soon\","
                                "\"giphy_api_key\":\"" P3A_SETTINGS_SECRET_MASK "\",\"giphy_rating\":\"r\"}");
    TEST_ASSERT_EQUAL(4, p3a_settings_load_json(&s, stored));
    cJSON_Delete(stored);
    TEST_ASSERT_EQUAL(40, s.brightness);
    TEST_ASSERT_EQUAL(P3A_GIPHY_RATING_R, s.giphy_rating);
    TEST_ASSERT_EQUAL(0, s.rotation);
    TEST_ASSERT_EQUAL(30, s.dwell_s);
    TEST_ASSERT_EQUAL_STRING("", s.giphy_api_key);

    cJSON *array = cJSON_Parse("[1]");
    TEST_ASSERT_EQUAL(0, p3a_settings_load_json(&s, array));
    cJSON_Delete(array);
    TEST_ASSERT_EQUAL(0, p3a_settings_load_json(&s, NULL));
}

// --- recovery policy ------------------------------------------------------------

static void test_recovery_policy(void)
{
    TEST_ASSERT_EQUAL(P3A_RECOVERY_CONTINUE, p3a_recovery_policy(0, true, 1));
    TEST_ASSERT_EQUAL(P3A_RECOVERY_CONTINUE, p3a_recovery_policy(5, true, 1));
    TEST_ASSERT_EQUAL(P3A_RECOVERY_REBOOT, p3a_recovery_policy(0, false, 1));
    TEST_ASSERT_EQUAL(P3A_RECOVERY_DEGRADED, p3a_recovery_policy(1, false, 1));
    TEST_ASSERT_EQUAL(P3A_RECOVERY_DEGRADED, p3a_recovery_policy(9, false, 1));
    TEST_ASSERT_EQUAL(P3A_RECOVERY_REBOOT, p3a_recovery_policy(2, false, 3));
    TEST_ASSERT_EQUAL(P3A_RECOVERY_DEGRADED, p3a_recovery_policy(3, false, 3));
    // Zero allowed restarts never restarts.
    TEST_ASSERT_EQUAL(P3A_RECOVERY_DEGRADED, p3a_recovery_policy(0, false, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fields_lie_inside_the_struct_and_do_not_overlap);
    RUN_TEST(test_every_key_is_found_and_ids_follow_the_table);
    RUN_TEST(test_defaults);
    RUN_TEST(test_reboot_advanced_and_secret_flags_come_from_the_schema);
    RUN_TEST(test_apply_one_of_each_type);
    RUN_TEST(test_writing_the_value_already_held_changes_nothing);
    RUN_TEST(test_one_bad_entry_refuses_the_whole_request);
    RUN_TEST(test_unknown_key_is_refused_and_named);
    RUN_TEST(test_a_hostile_key_cannot_overflow_or_corrupt_the_message);
    RUN_TEST(test_not_an_object);
    RUN_TEST(test_bool_takes_only_true_or_false);
    RUN_TEST(test_int_range_edges);
    RUN_TEST(test_int_with_choices);
    RUN_TEST(test_enum_takes_only_listed_names);
    RUN_TEST(test_color);
    RUN_TEST(test_string_length_and_characters);
    RUN_TEST(test_device_name_rule);
    RUN_TEST(test_sd_root_rule);
    RUN_TEST(test_country_is_two_letters_stored_upper_case);
    RUN_TEST(test_secrets_are_masked_when_read_and_kept_when_the_mask_is_written_back);
    RUN_TEST(test_full_json_lists_every_setting);
    RUN_TEST(test_defaults_store_as_an_empty_object);
    RUN_TEST(test_storage_round_trip_of_every_setting);
    RUN_TEST(test_load_skips_what_it_cannot_use_and_keeps_the_rest);
    RUN_TEST(test_recovery_policy);
    return UNITY_END();
}
