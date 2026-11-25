// SPDX-License-Identifier: Apache-2.0
// The table of pin lists and the set of blobs the lists keep.
#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "p3a_pins.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static p3a_pin_table_t t;

static void test_the_first_list_is_active_and_slugs_are_never_shared(void)
{
    char a[P3A_PIN_SLUG_LEN], b[P3A_PIN_SLUG_LEN], c[P3A_PIN_SLUG_LEN];
    p3a_pin_table_init(&t);
    TEST_ASSERT_NULL(p3a_pin_table_active(&t));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_create(&t, "My Pins", 0, a));
    TEST_ASSERT_TRUE(p3a_pin_slug_ok(a));
    TEST_ASSERT_EQUAL_STRING(a, p3a_pin_table_active(&t)->slug);
    // The same name and salt again: another slug, and the first list stays active.
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_create(&t, "My Pins", 0, b));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_create(&t, "Caf\xC3\xA9 evenings", 7, c));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0 && strcmp(a, c) != 0 && strcmp(b, c) != 0);
    TEST_ASSERT_EQUAL(3, t.count);
    TEST_ASSERT_EQUAL_STRING(a, p3a_pin_table_active(&t)->slug);
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 evenings", p3a_pin_table_find(&t, c)->name);
    TEST_ASSERT_NULL(p3a_pin_table_find(&t, "00000000"));
    TEST_ASSERT_NULL(p3a_pin_table_find(&t, "nonsense"));
    TEST_ASSERT_NULL(p3a_pin_table_find(&t, NULL));
}

static void test_names_that_will_not_do_are_refused(void)
{
    char slug[P3A_PIN_SLUG_LEN];
    char long_name[P3A_PIN_NAME_LEN + 1];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';  // 65 bytes: one too many
    p3a_pin_table_init(&t);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_pin_table_create(&t, "", 0, slug));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_pin_table_create(&t, NULL, 0, slug));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_pin_table_create(&t, long_name, 0, slug));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_pin_table_create(&t, "two\nlines", 0, slug));
    TEST_ASSERT_EQUAL(0, t.count);
    long_name[P3A_PIN_NAME_LEN - 1] = '\0';  // 64 bytes fit
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_create(&t, long_name, 0, slug));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_pin_table_rename(&t, slug, ""));
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, p3a_pin_table_rename(&t, "0badf00d", "x"));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_rename(&t, slug, "Short"));
    TEST_ASSERT_EQUAL_STRING("Short", p3a_pin_table_find(&t, slug)->name);
    // Nothing of the longer name is left behind in the field.
    static const char zeros[P3A_PIN_NAME_LEN - 6] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zeros, p3a_pin_table_find(&t, slug)->name + 6, sizeof(zeros));
}

static void test_the_active_list_cannot_be_deleted_and_the_table_holds_64(void)
{
    char first[P3A_PIN_SLUG_LEN], second[P3A_PIN_SLUG_LEN], slug[P3A_PIN_SLUG_LEN];
    p3a_pin_table_init(&t);
    p3a_pin_table_create(&t, "One", 0, first);
    p3a_pin_table_create(&t, "Two", 0, second);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, p3a_pin_table_delete(&t, first));
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, p3a_pin_table_delete(&t, "0badf00d"));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_set_active(&t, second));
    TEST_ASSERT_EQUAL_STRING(second, p3a_pin_table_active(&t)->slug);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_delete(&t, first));
    TEST_ASSERT_EQUAL(1, t.count);
    TEST_ASSERT_NULL(p3a_pin_table_find(&t, first));
    TEST_ASSERT_EQUAL(P3A_ERR_MISSING, p3a_pin_table_set_active(&t, first));
    for (int i = 1; i < P3A_PINS_MAX_LISTS; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_create(&t, "Same name", (uint32_t)i, slug));
    }
    TEST_ASSERT_EQUAL(P3A_PINS_MAX_LISTS, t.count);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, p3a_pin_table_create(&t, "One more", 0, slug));
    // Exactly one is active, still the one chosen.
    uint32_t active = 0;
    for (uint32_t i = 0; i < t.count; i++) {
        active += t.lists[i].active;
    }
    TEST_ASSERT_EQUAL(1, active);
    TEST_ASSERT_EQUAL_STRING(second, p3a_pin_table_active(&t)->slug);
}

static void test_a_table_from_a_file_is_checked_whole(void)
{
    static p3a_pin_table_t made;
    static p3a_pin_list_t file[3];
    char slug[P3A_PIN_SLUG_LEN];
    p3a_pin_table_init(&made);
    p3a_pin_table_create(&made, "One", 0, slug);
    p3a_pin_table_create(&made, "Two", 0, slug);
    p3a_pin_table_create(&made, "Three", 0, slug);
    p3a_pin_table_set_active(&made, made.lists[1].slug);

    memcpy(file, made.lists, sizeof(file));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_set(&t, file, 3));
    TEST_ASSERT_EQUAL(3, t.count);
    TEST_ASSERT_EQUAL_STRING("Two", p3a_pin_table_active(&t)->name);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_set(&t, file, 0));
    TEST_ASSERT_EQUAL(0, t.count);

    // No list marked active, or two: the first is.
    file[1].active = 0;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_set(&t, file, 3));
    TEST_ASSERT_EQUAL_STRING("One", p3a_pin_table_active(&t)->name);
    file[1].active = 1;
    file[2].active = 1;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_table_set(&t, file, 3));
    TEST_ASSERT_EQUAL_STRING("One", p3a_pin_table_active(&t)->name);

    // Each kind of damage refuses the lot.
    static const struct {
        int entry, offset;
        uint8_t value;
        const char *what;
    } damage[] = {
        {0, 0, 'G', "a slug that is not hex"},
        {1, 8, 'x', "a slug with no terminator"},
        {2, 9, 0, "an empty name"},
        {0, 9 + 64, 'x', "a name with no terminator"},
        {1, 9 + 20, 'x', "bytes after the name's terminator"},
        {2, 9, 7, "a control character in a name"},
        {0, 74, 2, "an active flag that is neither 0 nor 1"},
        {1, 79, 1, "a byte in the padding"},
    };
    for (size_t i = 0; i < sizeof(damage) / sizeof(damage[0]); i++) {
        memcpy(file, made.lists, sizeof(file));
        ((uint8_t *)&file[damage[i].entry])[damage[i].offset] = damage[i].value;
        TEST_ASSERT_EQUAL_MESSAGE(P3A_ERR_CORRUPT, p3a_pin_table_set(&t, file, 3), damage[i].what);
        TEST_ASSERT_EQUAL(0, t.count);
    }
    memcpy(file, made.lists, sizeof(file));
    memcpy(file[2].slug, file[0].slug, sizeof(file[0].slug));
    TEST_ASSERT_EQUAL_MESSAGE(P3A_ERR_CORRUPT, p3a_pin_table_set(&t, file, 3), "the same slug twice");
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_pin_table_set(&t, file, P3A_PINS_MAX_LISTS + 1));
}

static void ref_of(uint8_t ref[P3A_RECORD_BLOB_REF_LEN], uint32_t n)
{
    memset(ref, 0, P3A_RECORD_BLOB_REF_LEN);
    // Spread over the first bytes, so that order in the set is not the order added.
    ref[0] = (uint8_t)(n * 151u);
    ref[1] = (uint8_t)(n * 31u >> 3);
    ref[14] = (uint8_t)(n >> 8);
    ref[15] = (uint8_t)n;
}

static void test_the_kept_blobs_are_found_whatever_the_order_and_counted_per_pinning(void)
{
    p3a_pin_keep_t keep = {0};
    uint8_t ref[P3A_RECORD_BLOB_REF_LEN], zero[P3A_RECORD_BLOB_REF_LEN] = {0};
    TEST_ASSERT_FALSE(p3a_pin_keep_has(&keep, zero));
    for (uint32_t n = 1; n <= 1000; n++) {  // past several growths
        ref_of(ref, n);
        TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_keep_add(&keep, ref));
    }
    TEST_ASSERT_EQUAL(1000, keep.count);
    for (uint32_t i = 1; i < keep.count; i++) {
        TEST_ASSERT_TRUE(memcmp(keep.refs[i - 1], keep.refs[i], P3A_RECORD_BLOB_REF_LEN) <= 0);
    }
    for (uint32_t n = 1; n <= 1000; n++) {
        ref_of(ref, n);
        TEST_ASSERT_TRUE(p3a_pin_keep_has(&keep, ref));
    }
    ref_of(ref, 2000);
    TEST_ASSERT_FALSE(p3a_pin_keep_has(&keep, ref));
    p3a_pin_keep_remove(&keep, ref);  // not there: nothing happens
    TEST_ASSERT_EQUAL(1000, keep.count);
    // "No blob" is never kept.
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_keep_add(&keep, zero));
    TEST_ASSERT_EQUAL(1000, keep.count);
    TEST_ASSERT_FALSE(p3a_pin_keep_has(&keep, zero));

    // The same artwork in two lists is kept until both let go.
    ref_of(ref, 500);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_pin_keep_add(&keep, ref));
    TEST_ASSERT_EQUAL(1001, keep.count);
    p3a_pin_keep_remove(&keep, ref);
    TEST_ASSERT_TRUE(p3a_pin_keep_has(&keep, ref));
    p3a_pin_keep_remove(&keep, ref);
    TEST_ASSERT_FALSE(p3a_pin_keep_has(&keep, ref));
    TEST_ASSERT_EQUAL(999, keep.count);
    // Its neighbours are untouched.
    ref_of(ref, 499);
    TEST_ASSERT_TRUE(p3a_pin_keep_has(&keep, ref));
    ref_of(ref, 501);
    TEST_ASSERT_TRUE(p3a_pin_keep_has(&keep, ref));

    for (uint32_t n = 1; n <= 1000; n++) {
        ref_of(ref, n);
        p3a_pin_keep_remove(&keep, ref);
    }
    TEST_ASSERT_EQUAL(0, keep.count);
    p3a_pin_keep_free(&keep);
    TEST_ASSERT_NULL(keep.refs);
    p3a_pin_keep_free(&keep);  // twice is fine
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_list_is_active_and_slugs_are_never_shared);
    RUN_TEST(test_names_that_will_not_do_are_refused);
    RUN_TEST(test_the_active_list_cannot_be_deleted_and_the_table_holds_64);
    RUN_TEST(test_a_table_from_a_file_is_checked_whole);
    RUN_TEST(test_the_kept_blobs_are_found_whatever_the_order_and_counted_per_pinning);
    return UNITY_END();
}
