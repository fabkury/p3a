// SPDX-License-Identifier: Apache-2.0
#include "p3a_err.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_every_code_has_a_distinct_name(void)
{
    const char *seen[P3A_ERR_END_ - P3A_ERR_BASE] = {0};
    int count = 0;
    for (int code = P3A_ERR_BASE + 1; code < P3A_ERR_END_; code++) {
        const char *name = p3a_err_name(code);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strlen(name) > 0);
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(seen[i], name) != 0, name);
        }
        seen[count++] = name;
        TEST_ASSERT_TRUE(p3a_err_is_p3a(code));
        TEST_ASSERT_EQUAL_STRING(name, p3a_err_to_name(code));
    }
    TEST_ASSERT_EQUAL_INT(7, count);
}

static void test_names_are_the_stable_ones(void)
{
    TEST_ASSERT_EQUAL_STRING("NOT_PLAYABLE", p3a_err_name(P3A_ERR_NOT_PLAYABLE));
    TEST_ASSERT_EQUAL_STRING("CORRUPT", p3a_err_name(P3A_ERR_CORRUPT));
    TEST_ASSERT_EQUAL_STRING("MISSING", p3a_err_name(P3A_ERR_MISSING));
    TEST_ASSERT_EQUAL_STRING("RATE_LIMITED", p3a_err_name(P3A_ERR_RATE_LIMITED));
    TEST_ASSERT_EQUAL_STRING("AUTH", p3a_err_name(P3A_ERR_AUTH));
    TEST_ASSERT_EQUAL_STRING("NO_KEY", p3a_err_name(P3A_ERR_NO_KEY));
    TEST_ASSERT_EQUAL_STRING("SD_FAILED", p3a_err_name(P3A_ERR_SD_FAILED));
}

static void test_other_codes_are_not_p3a(void)
{
    static const esp_err_t others[] = {
        ESP_OK, ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT, P3A_ERR_BASE, P3A_ERR_END_, P3A_ERR_END_ + 1, -P3A_ERR_CORRUPT,
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        TEST_ASSERT_FALSE(p3a_err_is_p3a(others[i]));
        TEST_ASSERT_NULL(p3a_err_name(others[i]));
        TEST_ASSERT_NOT_NULL(p3a_err_to_name(others[i]));
    }
    TEST_ASSERT_EQUAL_STRING("ESP_ERR_NO_MEM", p3a_err_to_name(ESP_ERR_NO_MEM));
}

static void test_base_is_clear_of_idf_ranges(void)
{
    TEST_ASSERT_TRUE(P3A_ERR_BASE >= 0x10000);
    TEST_ASSERT_TRUE(P3A_ERR_END_ < 0x61000);  // esp_hidd
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_code_has_a_distinct_name);
    RUN_TEST(test_names_are_the_stable_ones);
    RUN_TEST(test_other_codes_are_not_p3a);
    RUN_TEST(test_base_is_clear_of_idf_ranges);
    return UNITY_END();
}
