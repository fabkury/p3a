// SPDX-License-Identifier: Apache-2.0
#include "p3a_clock.h"
#include "unity.h"

#include <string.h>

#include "net_backoff.h"

static char buf[32];

void setUp(void)
{
    memset(buf, 'x', sizeof(buf));
}
void tearDown(void) {}

static const char *fmt(long long t)
{
    TEST_ASSERT_EQUAL(ESP_OK, p3a_clock_format_iso8601((time_t)t, buf, sizeof(buf)));
    return buf;
}

// Expected values from Python's datetime.fromtimestamp(t, timezone.utc).
static void test_known_instants(void)
{
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", fmt(0));
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:01Z", fmt(1));
    TEST_ASSERT_EQUAL_STRING("2000-02-29T23:59:59Z", fmt(951868799));
    TEST_ASSERT_EQUAL_STRING("2000-03-01T00:00:00Z", fmt(951868800));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T12:00:00Z", fmt(1709208000));
    TEST_ASSERT_EQUAL_STRING("2026-11-18T14:03:22Z", fmt(1795010602));
    TEST_ASSERT_EQUAL_STRING("2026-12-31T23:59:59Z", fmt(1798761599));
    TEST_ASSERT_EQUAL_STRING("2027-01-01T00:00:00Z", fmt(1798761600));
    TEST_ASSERT_EQUAL_STRING("2038-01-19T03:14:07Z", fmt(2147483647));
    TEST_ASSERT_EQUAL_STRING("2100-03-01T00:00:00Z", fmt(4107542400LL));  // 2100 is not a leap year
}

static void test_before_1970(void)
{
    TEST_ASSERT_EQUAL_STRING("1969-12-31T23:59:59Z", fmt(-1));
    TEST_ASSERT_EQUAL_STRING("1969-12-31T00:00:00Z", fmt(-86400));
}

static void test_buffer_too_small_is_refused_untouched(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_clock_format_iso8601(0, buf, P3A_CLOCK_ISO8601_LEN - 1));
    TEST_ASSERT_EQUAL('x', buf[0]);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_clock_format_iso8601(0, NULL, 64));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_clock_format_iso8601(0, buf, P3A_CLOCK_ISO8601_LEN));
    TEST_ASSERT_EQUAL(20, strlen(buf));
}

static void test_now_is_refused_until_the_clock_is_set(void)
{
    TEST_ASSERT_FALSE(p3a_clock_is_set());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, p3a_clock_now_iso8601(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL('x', buf[0]);
    p3a_clock_mark_set();
    TEST_ASSERT_TRUE(p3a_clock_is_set());
    TEST_ASSERT_EQUAL(ESP_OK, p3a_clock_now_iso8601(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(20, strlen(buf));
    TEST_ASSERT_EQUAL('Z', buf[19]);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_clock_now_iso8601(buf, 10));
}

// --- the Wi-Fi reconnect delay (components/p3a_net/include/net_backoff.h) ---

static void test_backoff_schedule(void)
{
    // Never connected since boot: no waiting.
    for (int attempt = 1; attempt <= 12; attempt++) {
        TEST_ASSERT_EQUAL_UINT32(0, net_backoff_delay_ms(attempt, false));
    }
    static const uint32_t expected[] = {0, 0, 0, 2000, 4000, 8000, 16000, 20000, 20000, 20000};
    uint32_t total = 0;
    for (int attempt = 1; attempt <= 10; attempt++) {
        TEST_ASSERT_EQUAL_UINT32(expected[attempt - 1], net_backoff_delay_ms(attempt, true));
        total += net_backoff_delay_ms(attempt, true);
    }
    TEST_ASSERT_EQUAL_UINT32(90000, total);  // plus the time each attempt takes: about 97 s
}

static void test_backoff_is_capped_and_never_overflows(void)
{
    TEST_ASSERT_EQUAL_UINT32(20000, net_backoff_delay_ms(40, true));
    TEST_ASSERT_EQUAL_UINT32(20000, net_backoff_delay_ms(1000000, true));
    TEST_ASSERT_EQUAL_UINT32(20000, net_backoff_delay_ms(0x7FFFFFFF, true));
    TEST_ASSERT_EQUAL_UINT32(0, net_backoff_delay_ms(0, true));
    TEST_ASSERT_EQUAL_UINT32(0, net_backoff_delay_ms(-5, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_instants);
    RUN_TEST(test_before_1970);
    RUN_TEST(test_buffer_too_small_is_refused_untouched);
    RUN_TEST(test_now_is_refused_until_the_clock_is_set);
    RUN_TEST(test_backoff_schedule);
    RUN_TEST(test_backoff_is_capped_and_never_overflows);
    return UNITY_END();
}
