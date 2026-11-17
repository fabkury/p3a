// SPDX-License-Identifier: Apache-2.0
#include "p3a_event_table.h"
#include "unity.h"

static p3a_event_table_t t;
static p3a_event_sub_t out[P3A_EVENT_MAX_SUBSCRIBERS];

static void handler_a(const p3a_event_t *e, void *ctx)
{
    (void)e;
    (void)ctx;
}
static void handler_b(const p3a_event_t *e, void *ctx)
{
    (void)e;
    (void)ctx;
}

void setUp(void)
{
    p3a_event_table_init(&t);
}
void tearDown(void) {}

static void test_match_returns_only_that_type_in_subscription_order(void)
{
    int x = 0, y = 0;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_a, &x));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 20, handler_a, &x));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_b, &y));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_a, &y));

    size_t n = p3a_event_table_match(&t, 10, out, P3A_EVENT_MAX_SUBSCRIBERS);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_TRUE(out[0].handler == handler_a && out[0].ctx == &x);
    TEST_ASSERT_TRUE(out[1].handler == handler_b && out[1].ctx == &y);
    TEST_ASSERT_TRUE(out[2].handler == handler_a && out[2].ctx == &y);
    TEST_ASSERT_EQUAL_size_t(1, p3a_event_table_match(&t, 20, out, P3A_EVENT_MAX_SUBSCRIBERS));
    TEST_ASSERT_EQUAL_size_t(0, p3a_event_table_match(&t, 30, out, P3A_EVENT_MAX_SUBSCRIBERS));
}

static void test_duplicate_registration_is_refused(void)
{
    int x = 0;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_a, &x));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, p3a_event_table_add(&t, 10, handler_a, &x));
    TEST_ASSERT_EQUAL_size_t(1, p3a_event_table_match(&t, 10, out, P3A_EVENT_MAX_SUBSCRIBERS));
    // A different ctx or type is a different registration.
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 11, handler_a, &x));
}

static void test_null_handler_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_event_table_add(&t, 10, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0, t.count);
}

static void test_remove_keeps_the_order_of_the_rest(void)
{
    int c[4];
    for (int i = 0; i < 4; i++) {
        p3a_event_table_add(&t, 10, handler_a, &c[i]);
    }
    TEST_ASSERT_TRUE(p3a_event_table_remove(&t, 10, handler_a, &c[1]));
    TEST_ASSERT_FALSE(p3a_event_table_remove(&t, 10, handler_a, &c[1]));  // already gone
    TEST_ASSERT_FALSE(p3a_event_table_remove(&t, 99, handler_a, &c[0]));  // wrong type
    TEST_ASSERT_FALSE(p3a_event_table_remove(&t, 10, handler_b, &c[0]));  // wrong handler

    size_t n = p3a_event_table_match(&t, 10, out, P3A_EVENT_MAX_SUBSCRIBERS);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_PTR(&c[0], out[0].ctx);
    TEST_ASSERT_EQUAL_PTR(&c[2], out[1].ctx);
    TEST_ASSERT_EQUAL_PTR(&c[3], out[2].ctx);

    TEST_ASSERT_TRUE(p3a_event_table_remove(&t, 10, handler_a, &c[3]));  // the last one
    TEST_ASSERT_TRUE(p3a_event_table_remove(&t, 10, handler_a, &c[0]));  // the first one
    TEST_ASSERT_EQUAL_size_t(1, p3a_event_table_match(&t, 10, out, P3A_EVENT_MAX_SUBSCRIBERS));
    TEST_ASSERT_EQUAL_PTR(&c[2], out[0].ctx);
}

static void test_full_table(void)
{
    static int ctx[P3A_EVENT_MAX_SUBSCRIBERS + 1];
    for (int i = 0; i < P3A_EVENT_MAX_SUBSCRIBERS; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_a, &ctx[i]));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, p3a_event_table_add(&t, 10, handler_a, &ctx[P3A_EVENT_MAX_SUBSCRIBERS]));
    TEST_ASSERT_EQUAL_size_t(P3A_EVENT_MAX_SUBSCRIBERS, p3a_event_table_match(&t, 10, out, P3A_EVENT_MAX_SUBSCRIBERS));
    // Removing one makes room again.
    TEST_ASSERT_TRUE(p3a_event_table_remove(&t, 10, handler_a, &ctx[7]));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_event_table_add(&t, 10, handler_a, &ctx[P3A_EVENT_MAX_SUBSCRIBERS]));
}

static void test_match_respects_the_output_limit(void)
{
    int c[5];
    for (int i = 0; i < 5; i++) {
        p3a_event_table_add(&t, 10, handler_a, &c[i]);
    }
    TEST_ASSERT_EQUAL_size_t(2, p3a_event_table_match(&t, 10, out, 2));
    TEST_ASSERT_EQUAL_PTR(&c[1], out[1].ctx);
    TEST_ASSERT_EQUAL_size_t(0, p3a_event_table_match(&t, 10, out, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_match_returns_only_that_type_in_subscription_order);
    RUN_TEST(test_duplicate_registration_is_refused);
    RUN_TEST(test_null_handler_is_refused);
    RUN_TEST(test_remove_keeps_the_order_of_the_rest);
    RUN_TEST(test_full_table);
    RUN_TEST(test_match_respects_the_output_limit);
    return UNITY_END();
}
