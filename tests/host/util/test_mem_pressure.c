// SPDX-License-Identifier: Apache-2.0
#include "p3a_mem.h"
#include "unity.h"

// Round numbers so the expectations can be read: LOW below 100/50, CRITICAL
// below 40/20, and a 25 percent margin to recover (125/62 and 50/25).
static const p3a_mem_thresholds_t T = {
    .low_free_bytes = 100,
    .low_largest_block = 50,
    .critical_free_bytes = 40,
    .critical_largest_block = 20,
    .recover_margin_percent = 25,
};

void setUp(void) {}
void tearDown(void) {}

static p3a_mem_level_t eval(p3a_mem_level_t previous, uint32_t free_bytes, uint32_t largest)
{
    return p3a_mem_evaluate(previous, free_bytes, largest, &T);
}

static void test_plenty_of_memory_is_ok(void)
{
    TEST_ASSERT_EQUAL(P3A_MEM_OK, eval(P3A_MEM_OK, 1000, 500));
    TEST_ASSERT_EQUAL(P3A_MEM_OK, eval(P3A_MEM_OK, 100, 50));  // at the threshold is not below it
}

static void test_either_number_can_trip_a_level(void)
{
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_OK, 99, 500));   // little free
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_OK, 1000, 49));  // fragmented: plenty free, no big block
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_OK, 39, 500));
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_OK, 1000, 19));
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_OK, 0, 0));
}

static void test_getting_worse_is_immediate(void)
{
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_OK, 30, 30));   // straight past LOW
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_LOW, 39, 500));
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_LOW, 99, 500));
}

static void test_recovery_from_low_needs_the_margin(void)
{
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_LOW, 100, 50));    // back at the threshold: not enough
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_LOW, 124, 500));   // free just short of 125
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_LOW, 1000, 61));   // block just short of 62
    TEST_ASSERT_EQUAL(P3A_MEM_OK, eval(P3A_MEM_LOW, 125, 62));
}

static void test_recovery_from_critical_steps_through_low(void)
{
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_CRITICAL, 45, 500));  // above 40, below 50
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, eval(P3A_MEM_CRITICAL, 1000, 24));
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_CRITICAL, 50, 25));        // clear of CRITICAL, still LOW
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, eval(P3A_MEM_CRITICAL, 110, 55));       // above LOW, but not by the margin
    TEST_ASSERT_EQUAL(P3A_MEM_OK, eval(P3A_MEM_CRITICAL, 1000, 500));      // a big release goes all the way
}

static void test_no_flapping_around_a_threshold(void)
{
    // Free memory wobbling by a few bytes around the LOW threshold changes
    // the level once, not on every sample.
    p3a_mem_level_t level = P3A_MEM_OK;
    int changes = 0;
    for (int i = 0; i < 50; i++) {
        uint32_t free_bytes = (i % 2) ? 97 : 103;
        p3a_mem_level_t next = eval(level, free_bytes, 500);
        changes += next != level;
        level = next;
    }
    TEST_ASSERT_EQUAL_INT(1, changes);
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, level);
}

static void test_zero_margin_means_no_hysteresis(void)
{
    p3a_mem_thresholds_t t = T;
    t.recover_margin_percent = 0;
    TEST_ASSERT_EQUAL(P3A_MEM_OK, p3a_mem_evaluate(P3A_MEM_LOW, 100, 50, &t));
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, p3a_mem_evaluate(P3A_MEM_CRITICAL, 40, 20, &t));
}

static void test_default_thresholds_are_ordered_and_fit_the_pool(void)
{
    p3a_mem_thresholds_t d = p3a_mem_default_thresholds();
    TEST_ASSERT_TRUE(d.critical_free_bytes < d.low_free_bytes);
    TEST_ASSERT_TRUE(d.critical_largest_block < d.low_largest_block);
    TEST_ASSERT_TRUE(d.low_largest_block <= d.low_free_bytes);
    TEST_ASSERT_TRUE(d.critical_largest_block <= d.critical_free_bytes);
    // The 1.x crash snapshot (about 30 KB free, largest block 8 KB) must read as CRITICAL.
    TEST_ASSERT_EQUAL(P3A_MEM_CRITICAL, p3a_mem_evaluate(P3A_MEM_OK, 30 * 1024, 8 * 1024, &d));
    // A freshly booted pool (about 272 KB, mostly free) must read as OK.
    TEST_ASSERT_EQUAL(P3A_MEM_OK, p3a_mem_evaluate(P3A_MEM_OK, 200 * 1024, 150 * 1024, &d));
    // And the recovery thresholds must not exceed the pool.
    TEST_ASSERT_TRUE((uint64_t)d.low_free_bytes * (100u + d.recover_margin_percent) / 100u < 272u * 1024u);
}

static void test_margin_arithmetic_does_not_overflow(void)
{
    p3a_mem_thresholds_t t = {
        .low_free_bytes = 4000000000u,
        .low_largest_block = 1,
        .critical_free_bytes = 1,
        .critical_largest_block = 1,
        .recover_margin_percent = 100,
    };
    // 4e9 * 2 does not fit in 32 bits; the comparison must still be sane.
    TEST_ASSERT_EQUAL(P3A_MEM_LOW, p3a_mem_evaluate(P3A_MEM_LOW, 4100000000u, 10, &t));
}

static void test_level_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", p3a_mem_level_name(P3A_MEM_OK));
    TEST_ASSERT_EQUAL_STRING("low", p3a_mem_level_name(P3A_MEM_LOW));
    TEST_ASSERT_EQUAL_STRING("critical", p3a_mem_level_name(P3A_MEM_CRITICAL));
    TEST_ASSERT_EQUAL_STRING("invalid", p3a_mem_level_name((p3a_mem_level_t)9));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plenty_of_memory_is_ok);
    RUN_TEST(test_either_number_can_trip_a_level);
    RUN_TEST(test_getting_worse_is_immediate);
    RUN_TEST(test_recovery_from_low_needs_the_margin);
    RUN_TEST(test_recovery_from_critical_steps_through_low);
    RUN_TEST(test_no_flapping_around_a_threshold);
    RUN_TEST(test_zero_margin_means_no_hysteresis);
    RUN_TEST(test_default_thresholds_are_ordered_and_fit_the_pool);
    RUN_TEST(test_margin_arithmetic_does_not_overflow);
    RUN_TEST(test_level_names);
    return UNITY_END();
}
