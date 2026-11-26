// SPDX-License-Identifier: Apache-2.0
#include "p3a_job_table.h"
#include "unity.h"

#include <string.h>

static p3a_job_table_t t;

void setUp(void)
{
    p3a_job_table_init(&t);
}
void tearDown(void) {}

static p3a_job_info_t info(p3a_job_id_t id)
{
    p3a_job_info_t out;
    TEST_ASSERT_TRUE(p3a_job_table_get(&t, id, &out));
    return out;
}

static void test_a_job_goes_queued_running_done(void)
{
    p3a_job_id_t id = p3a_job_table_add(&t, "ota_install");
    TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, id);
    TEST_ASSERT_EQUAL(P3A_JOB_QUEUED, info(id).state);
    TEST_ASSERT_EQUAL_STRING("ota_install", info(id).name);
    TEST_ASSERT_EQUAL_UINT8(0, info(id).progress);

    TEST_ASSERT_TRUE(p3a_job_table_start(&t, id));
    TEST_ASSERT_EQUAL(P3A_JOB_RUNNING, info(id).state);
    TEST_ASSERT_TRUE(p3a_job_table_progress(&t, id, 40));
    TEST_ASSERT_EQUAL_UINT8(40, info(id).progress);

    TEST_ASSERT_TRUE(p3a_job_table_finish(&t, id, ESP_OK));
    TEST_ASSERT_EQUAL(P3A_JOB_DONE, info(id).state);
    TEST_ASSERT_EQUAL(ESP_OK, info(id).result);
    TEST_ASSERT_EQUAL_UINT8(100, info(id).progress);
}

static void test_a_failed_job_keeps_its_progress_and_result(void)
{
    p3a_job_id_t id = p3a_job_table_add(&t, "x");
    p3a_job_table_start(&t, id);
    p3a_job_table_progress(&t, id, 63);
    TEST_ASSERT_TRUE(p3a_job_table_finish(&t, id, ESP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, info(id).result);
    TEST_ASSERT_EQUAL_UINT8(63, info(id).progress);
}

static void test_ids_are_distinct_and_never_zero(void)
{
    p3a_job_id_t a = p3a_job_table_add(&t, "a");
    p3a_job_id_t b = p3a_job_table_add(&t, "b");
    TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, a);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_EQUAL_STRING("b", info(b).name);
}

static void test_invalid_transitions_change_nothing(void)
{
    p3a_job_id_t id = p3a_job_table_add(&t, "x");
    TEST_ASSERT_FALSE(p3a_job_table_progress(&t, id, 10));  // not running yet
    TEST_ASSERT_TRUE(p3a_job_table_start(&t, id));
    TEST_ASSERT_FALSE(p3a_job_table_start(&t, id));          // already running
    TEST_ASSERT_TRUE(p3a_job_table_finish(&t, id, ESP_FAIL));
    TEST_ASSERT_FALSE(p3a_job_table_finish(&t, id, ESP_OK)); // a result is final
    TEST_ASSERT_FALSE(p3a_job_table_progress(&t, id, 99));
    TEST_ASSERT_FALSE(p3a_job_table_start(&t, id));
    TEST_ASSERT_EQUAL(ESP_FAIL, info(id).result);
}

static void test_a_queued_job_can_be_finished_without_running(void)
{
    // A job dropped before it ran (queue full, shutdown) still gets a result.
    p3a_job_id_t id = p3a_job_table_add(&t, "x");
    TEST_ASSERT_TRUE(p3a_job_table_finish(&t, id, ESP_ERR_NO_MEM));
    TEST_ASSERT_EQUAL(P3A_JOB_DONE, info(id).state);
}

static void test_unknown_ids(void)
{
    p3a_job_info_t out;
    TEST_ASSERT_FALSE(p3a_job_table_get(&t, P3A_JOB_ID_NONE, &out));
    TEST_ASSERT_FALSE(p3a_job_table_get(&t, 12345, &out));
    TEST_ASSERT_FALSE(p3a_job_table_start(&t, 12345));
    TEST_ASSERT_FALSE(p3a_job_table_progress(&t, 12345, 1));
    TEST_ASSERT_FALSE(p3a_job_table_finish(&t, P3A_JOB_ID_NONE, ESP_OK));
    TEST_ASSERT_TRUE(p3a_job_table_get(&t, p3a_job_table_add(&t, "x"), NULL));  // NULL out is allowed
}

static void test_progress_is_capped_at_100(void)
{
    p3a_job_id_t id = p3a_job_table_add(&t, "x");
    p3a_job_table_start(&t, id);
    TEST_ASSERT_TRUE(p3a_job_table_progress(&t, id, 250));
    TEST_ASSERT_EQUAL_UINT8(100, info(id).progress);
}

static void test_long_and_missing_names(void)
{
    p3a_job_id_t id = p3a_job_table_add(&t, "a-name-that-is-much-longer-than-the-field");
    TEST_ASSERT_EQUAL_size_t(P3A_JOB_NAME_MAX - 1, strlen(info(id).name));
    id = p3a_job_table_add(&t, NULL);
    TEST_ASSERT_EQUAL_STRING("", info(id).name);
}

static void test_full_table_of_live_jobs_refuses_more(void)
{
    for (int i = 0; i < P3A_JOB_TABLE_SIZE; i++) {
        TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, p3a_job_table_add(&t, "live"));
    }
    TEST_ASSERT_EQUAL(P3A_JOB_ID_NONE, p3a_job_table_add(&t, "one too many"));
}

static void test_oldest_finished_job_is_replaced_first(void)
{
    p3a_job_id_t ids[P3A_JOB_TABLE_SIZE];
    for (int i = 0; i < P3A_JOB_TABLE_SIZE; i++) {
        ids[i] = p3a_job_table_add(&t, "j");
    }
    // Finish three, out of order. The one with the lowest id is the oldest.
    p3a_job_table_finish(&t, ids[9], ESP_OK);
    p3a_job_table_finish(&t, ids[2], ESP_OK);
    p3a_job_table_finish(&t, ids[5], ESP_OK);

    p3a_job_id_t fresh = p3a_job_table_add(&t, "new");
    TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, fresh);
    TEST_ASSERT_FALSE(p3a_job_table_get(&t, ids[2], NULL));  // replaced
    TEST_ASSERT_TRUE(p3a_job_table_get(&t, ids[5], NULL));
    TEST_ASSERT_TRUE(p3a_job_table_get(&t, ids[9], NULL));
    TEST_ASSERT_EQUAL(P3A_JOB_QUEUED, info(fresh).state);
    TEST_ASSERT_EQUAL_UINT8(0, info(fresh).progress);

    // Live jobs are never replaced.
    for (int i = 0; i < P3A_JOB_TABLE_SIZE; i++) {
        if (i != 2 && i != 5 && i != 9) {
            TEST_ASSERT_EQUAL(P3A_JOB_QUEUED, info(ids[i]).state);
        }
    }
}

static void test_a_watched_job_outlives_the_ones_nobody_asked_about(void)
{
    // A web page was given this job's id, and it has finished.
    const p3a_job_id_t watched = p3a_job_table_add(&t, "asked for");
    TEST_ASSERT_TRUE(p3a_job_table_watch(&t, watched));
    p3a_job_table_finish(&t, watched, ESP_ERR_NOT_FOUND);
    // The device then runs many jobs of its own, more than the table holds.
    for (int i = 0; i < P3A_JOB_TABLE_SIZE * 5; i++) {
        const p3a_job_id_t own = p3a_job_table_add(&t, "own");
        TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, own);
        p3a_job_table_finish(&t, own, ESP_OK);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, info(watched).result);
    TEST_ASSERT_TRUE(info(watched).watched);

    // Watched jobs make room for each other, the oldest first, once nothing else can go.
    p3a_job_id_t more[P3A_JOB_TABLE_SIZE];
    for (int i = 0; i < P3A_JOB_TABLE_SIZE; i++) {
        more[i] = p3a_job_table_add(&t, "asked for too");
        TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, more[i]);
        p3a_job_table_watch(&t, more[i]);
        p3a_job_table_finish(&t, more[i], ESP_OK);
    }
    TEST_ASSERT_FALSE(p3a_job_table_get(&t, watched, NULL));
    TEST_ASSERT_TRUE(p3a_job_table_get(&t, more[0], NULL));
    TEST_ASSERT_FALSE(p3a_job_table_watch(&t, P3A_JOB_ID_NONE));
    TEST_ASSERT_FALSE(p3a_job_table_watch(&t, 999999));
}

static void test_many_jobs_through_a_small_table(void)
{
    p3a_job_id_t previous = P3A_JOB_ID_NONE;
    for (int i = 0; i < 1000; i++) {
        p3a_job_id_t id = p3a_job_table_add(&t, "churn");
        TEST_ASSERT_NOT_EQUAL(P3A_JOB_ID_NONE, id);
        TEST_ASSERT_TRUE(id > previous);
        TEST_ASSERT_TRUE(p3a_job_table_start(&t, id));
        TEST_ASSERT_TRUE(p3a_job_table_finish(&t, id, ESP_OK));
        previous = id;
    }
    TEST_ASSERT_TRUE(p3a_job_table_get(&t, previous, NULL));
    TEST_ASSERT_FALSE(p3a_job_table_get(&t, previous - P3A_JOB_TABLE_SIZE, NULL));
}

static void test_id_wraps_past_zero(void)
{
    t.last_id = UINT32_MAX;
    p3a_job_id_t id = p3a_job_table_add(&t, "wrap");
    TEST_ASSERT_EQUAL_UINT32(1, id);
}

static void test_state_names(void)
{
    TEST_ASSERT_EQUAL_STRING("queued", p3a_job_state_name(P3A_JOB_QUEUED));
    TEST_ASSERT_EQUAL_STRING("running", p3a_job_state_name(P3A_JOB_RUNNING));
    TEST_ASSERT_EQUAL_STRING("done", p3a_job_state_name(P3A_JOB_DONE));
    TEST_ASSERT_EQUAL_STRING("invalid", p3a_job_state_name((p3a_job_state_t)7));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_job_goes_queued_running_done);
    RUN_TEST(test_a_failed_job_keeps_its_progress_and_result);
    RUN_TEST(test_ids_are_distinct_and_never_zero);
    RUN_TEST(test_invalid_transitions_change_nothing);
    RUN_TEST(test_a_queued_job_can_be_finished_without_running);
    RUN_TEST(test_unknown_ids);
    RUN_TEST(test_progress_is_capped_at_100);
    RUN_TEST(test_long_and_missing_names);
    RUN_TEST(test_full_table_of_live_jobs_refuses_more);
    RUN_TEST(test_oldest_finished_job_is_replaced_first);
    RUN_TEST(test_a_watched_job_outlives_the_ones_nobody_asked_about);
    RUN_TEST(test_many_jobs_through_a_small_table);
    RUN_TEST(test_id_wraps_past_zero);
    RUN_TEST(test_state_names);
    return UNITY_END();
}
