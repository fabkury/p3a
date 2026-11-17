// SPDX-License-Identifier: Apache-2.0
#include "p3a_version.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static p3a_version_t parsed(const char *text)
{
    p3a_version_t v;
    TEST_ASSERT_TRUE_MESSAGE(p3a_version_parse(text, &v), text);
    return v;
}

static int cmp(const char *a, const char *b)
{
    p3a_version_t va = parsed(a), vb = parsed(b);
    return p3a_version_compare(&va, &vb);
}

static void test_parses_plain_version(void)
{
    p3a_version_t v = parsed("2.0.0");
    TEST_ASSERT_EQUAL_UINT16(2, v.major);
    TEST_ASSERT_EQUAL_UINT16(0, v.minor);
    TEST_ASSERT_EQUAL_UINT16(0, v.patch);
    TEST_ASSERT_EQUAL_STRING("", v.suffix);
}

static void test_parses_tag_prefix_and_suffix(void)
{
    p3a_version_t v = parsed("v12.34.56-rc.1");
    TEST_ASSERT_EQUAL_UINT16(12, v.major);
    TEST_ASSERT_EQUAL_UINT16(34, v.minor);
    TEST_ASSERT_EQUAL_UINT16(56, v.patch);
    TEST_ASSERT_EQUAL_STRING("rc.1", v.suffix);
}

static void test_parses_largest_parts(void)
{
    p3a_version_t v = parsed("65535.65535.65535");
    TEST_ASSERT_EQUAL_UINT16(65535, v.patch);
}

static void test_rejects_malformed(void)
{
    static const char *const bad[] = {
        "",          "2",         "2.0",        "2.0.",    ".0.0",    "2..0",      "a.b.c",
        "2.0.0.1",   "2.0.0 ",    " 2.0.0",     "2.0.0-",  "2.0.0+x", "2.0.0-a b", "2.0.0-a_b",
        "65536.0.0", "0.0.65536", "100000.0.0", "vv2.0.0", "-1.0.0",  "2.0.0-abcdefghijklmnopqrstuvwx",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        p3a_version_t v;
        memset(&v, 0xAA, sizeof(v));
        p3a_version_t before = v;
        TEST_ASSERT_FALSE_MESSAGE(p3a_version_parse(bad[i], &v), bad[i]);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &v, sizeof(v), bad[i]);
    }
    p3a_version_t v;
    TEST_ASSERT_FALSE(p3a_version_parse(NULL, &v));
    TEST_ASSERT_FALSE(p3a_version_parse("2.0.0", NULL));
}

static void test_accepts_longest_suffix(void)
{
    p3a_version_t v = parsed("2.0.0-abcdefghijklmnopqrstuvw");  // 23 characters
    TEST_ASSERT_EQUAL_size_t(23, strlen(v.suffix));
}

static void test_orders_by_numbers(void)
{
    TEST_ASSERT_LESS_THAN_INT(0, cmp("1.2.4", "2.0.0"));
    TEST_ASSERT_LESS_THAN_INT(0, cmp("2.0.9", "2.1.0"));
    TEST_ASSERT_LESS_THAN_INT(0, cmp("2.0.9", "2.0.10"));  // numeric, not text
    TEST_ASSERT_GREATER_THAN_INT(0, cmp("2.10.0", "2.9.0"));
    TEST_ASSERT_EQUAL_INT(0, cmp("v2.0.0", "2.0.0"));
}

static void test_release_is_newer_than_prerelease(void)
{
    TEST_ASSERT_LESS_THAN_INT(0, cmp("2.0.0-dev", "2.0.0"));
    TEST_ASSERT_GREATER_THAN_INT(0, cmp("2.0.0", "2.0.0-rc1"));
    TEST_ASSERT_GREATER_THAN_INT(0, cmp("2.0.1-dev", "2.0.0"));  // numbers win over suffix
    TEST_ASSERT_LESS_THAN_INT(0, cmp("2.0.0-dev", "2.0.0-rc1"));
    TEST_ASSERT_EQUAL_INT(0, cmp("2.0.0-dev", "2.0.0-dev"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parses_plain_version);
    RUN_TEST(test_parses_tag_prefix_and_suffix);
    RUN_TEST(test_parses_largest_parts);
    RUN_TEST(test_rejects_malformed);
    RUN_TEST(test_accepts_longest_suffix);
    RUN_TEST(test_orders_by_numbers);
    RUN_TEST(test_release_is_newer_than_prerelease);
    return UNITY_END();
}
