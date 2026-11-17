// SPDX-License-Identifier: Apache-2.0
#include "p3a_task_policy.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_shipped_priorities(void)
{
    // What 1.x ran with in the field. Changing one is a jitter-relevant
    // decision; change this test with it, on purpose.
    TEST_ASSERT_EQUAL_UINT8(5, p3a_task_policy(P3A_TASK_RENDER).priority);
    TEST_ASSERT_EQUAL_UINT8(5, p3a_task_policy(P3A_TASK_TOUCH).priority);
    TEST_ASSERT_EQUAL_UINT8(4, p3a_task_policy(P3A_TASK_APP).priority);
    TEST_ASSERT_EQUAL_UINT8(3, p3a_task_policy(P3A_TASK_NETWORK).priority);
    TEST_ASSERT_EQUAL_UINT8(2, p3a_task_policy(P3A_TASK_BACKGROUND).priority);
}

static void test_nothing_outranks_render_and_background_is_lowest(void)
{
    uint8_t render = p3a_task_policy(P3A_TASK_RENDER).priority;
    uint8_t background = p3a_task_policy(P3A_TASK_BACKGROUND).priority;
    for (int c = 0; c < P3A_TASK_CLASS_COUNT; c++) {
        uint8_t p = p3a_task_policy((p3a_task_class_t)c).priority;
        TEST_ASSERT_TRUE(p <= render);
        TEST_ASSERT_TRUE(p >= background);
        TEST_ASSERT_TRUE(p >= 1);
    }
}

static void test_core_one_belongs_to_the_display_pipeline(void)
{
    TEST_ASSERT_EQUAL_INT8(1, p3a_task_policy(P3A_TASK_RENDER).core);
    TEST_ASSERT_EQUAL_INT8(0, p3a_task_policy(P3A_TASK_APP).core);
    TEST_ASSERT_EQUAL_INT8(0, p3a_task_policy(P3A_TASK_NETWORK).core);
    TEST_ASSERT_EQUAL_INT8(0, p3a_task_policy(P3A_TASK_BACKGROUND).core);
    for (int c = 0; c < P3A_TASK_CLASS_COUNT; c++) {
        if (c != P3A_TASK_RENDER) {
            TEST_ASSERT_NOT_EQUAL(1, p3a_task_policy((p3a_task_class_t)c).core);
        }
    }
}

static void test_only_render_keeps_its_stack_in_internal_ram(void)
{
    for (int c = 0; c < P3A_TASK_CLASS_COUNT; c++) {
        TEST_ASSERT_EQUAL(c != P3A_TASK_RENDER, p3a_task_policy((p3a_task_class_t)c).psram_stack);
    }
}

static void test_priority_offsets_are_clamped(void)
{
    TEST_ASSERT_EQUAL_UINT8(6, p3a_task_priority(P3A_TASK_RENDER, 1));  // the display consumer
    TEST_ASSERT_EQUAL_UINT8(4, p3a_task_priority(P3A_TASK_RENDER, -1));
    TEST_ASSERT_EQUAL_UINT8(1, p3a_task_priority(P3A_TASK_BACKGROUND, -5));
    TEST_ASSERT_EQUAL_UINT8(24, p3a_task_priority(P3A_TASK_RENDER, 100));
    TEST_ASSERT_EQUAL_UINT8(1, p3a_task_priority(P3A_TASK_RENDER, -100000));
}

static void test_out_of_range_class_is_treated_as_background(void)
{
    p3a_task_policy_t bad = p3a_task_policy((p3a_task_class_t)99);
    p3a_task_policy_t background = p3a_task_policy(P3A_TASK_BACKGROUND);
    TEST_ASSERT_EQUAL_MEMORY(&background, &bad, sizeof(bad));
    bad = p3a_task_policy((p3a_task_class_t)-1);
    TEST_ASSERT_EQUAL_MEMORY(&background, &bad, sizeof(bad));
    TEST_ASSERT_EQUAL_STRING("invalid", p3a_task_class_name((p3a_task_class_t)99));
    TEST_ASSERT_EQUAL_STRING("render", p3a_task_class_name(P3A_TASK_RENDER));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shipped_priorities);
    RUN_TEST(test_nothing_outranks_render_and_background_is_lowest);
    RUN_TEST(test_core_one_belongs_to_the_display_pipeline);
    RUN_TEST(test_only_render_keeps_its_stack_in_internal_ram);
    RUN_TEST(test_priority_offsets_are_clamped);
    RUN_TEST(test_out_of_range_class_is_treated_as_background);
    return UNITY_END();
}
