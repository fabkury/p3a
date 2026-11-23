// SPDX-License-Identifier: Apache-2.0
// Which blobs go when the card fills up.
#include <string.h>

#include "p3a_evict.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define MIB (1024ull * 1024)
#define GIB (1024ull * MIB)
#define NOW 1800000000u
#define HOUR 3600u

static void test_the_marks_follow_the_card_within_their_limits(void)
{
    uint64_t start, stop;
    TEST_ASSERT_FALSE(p3a_evict_marks(512 * MIB, &start, &stop));  // too small to evict from
    TEST_ASSERT_EQUAL_UINT64(0, start);
    TEST_ASSERT_TRUE(p3a_evict_marks(1 * GIB, &start, &stop));
    TEST_ASSERT_EQUAL_UINT64(256 * MIB, start);         // 10% would be 102 MiB: the floor
    TEST_ASSERT_EQUAL_UINT64((256 + 512) * MIB, stop);  // 25% would be 256 MiB: the floor
    TEST_ASSERT_TRUE(p3a_evict_marks(8 * GIB, &start, &stop));
    TEST_ASSERT_EQUAL_UINT64(8 * GIB / 10, start);
    TEST_ASSERT_EQUAL_UINT64(8 * GIB / 10 + 2 * GIB, stop);
    TEST_ASSERT_TRUE(p3a_evict_marks(32 * GIB, &start, &stop));
    TEST_ASSERT_EQUAL_UINT64(1 * GIB, start);  // the ceilings
    TEST_ASSERT_EQUAL_UINT64(5 * GIB, stop);
    TEST_ASSERT_TRUE(p3a_evict_marks(256 * GIB, &start, &stop));
    TEST_ASSERT_EQUAL_UINT64(5 * GIB, stop);
}

static p3a_evict_entry_t blob(uint8_t tag, uint32_t age_s, uint32_t size, bool keep)
{
    p3a_evict_entry_t e = {.modified_at = NOW - age_s, .size = size, .format = P3A_FORMAT_GIF, .keep = keep};
    memset(e.ref, tag, sizeof(e.ref));
    return e;
}

static void test_the_oldest_go_first_and_only_as_many_as_needed(void)
{
    p3a_evict_entry_t e[] = {blob(1, 5 * HOUR, 100, false), blob(2, 50 * HOUR, 100, false), blob(3, 9 * HOUR, 100, false),
                             blob(4, 20 * HOUR, 100, false)};
    uint64_t freed = 0;
    TEST_ASSERT_EQUAL(2, p3a_evict_choose(e, 4, 150, NOW, P3A_EVICT_MIN_AGE_S, &freed));
    TEST_ASSERT_EQUAL_UINT64(200, freed);
    TEST_ASSERT_EQUAL_HEX8(2, e[0].ref[0]);  // 50 hours
    TEST_ASSERT_EQUAL_HEX8(4, e[1].ref[0]);  // 20 hours
    TEST_ASSERT_EQUAL(0, p3a_evict_choose(e, 4, 0, NOW, P3A_EVICT_MIN_AGE_S, &freed));  // nothing needed
    TEST_ASSERT_EQUAL(1, p3a_evict_choose(e, 4, 100, NOW, P3A_EVICT_MIN_AGE_S, NULL));  // exactly enough
    TEST_ASSERT_EQUAL(0, p3a_evict_choose(e, 0, 100, NOW, P3A_EVICT_MIN_AGE_S, &freed));
    TEST_ASSERT_EQUAL_UINT64(0, freed);
}

static void test_young_and_kept_blobs_are_spared_whatever_is_needed(void)
{
    p3a_evict_entry_t e[] = {blob(1, 1 * HOUR, 100, false),   // young
                             blob(2, 90 * HOUR, 100, true),   // the oldest, but kept
                             blob(3, 9 * HOUR, 100, false),
                             blob(4, 4 * HOUR - 1, 100, false)};  // a second short of four hours
    uint64_t freed = 0;
    TEST_ASSERT_EQUAL(1, p3a_evict_choose(e, 4, 1000000, NOW, P3A_EVICT_MIN_AGE_S, &freed));
    TEST_ASSERT_EQUAL_UINT64(100, freed);  // less than asked: there was no more to give
    TEST_ASSERT_EQUAL_HEX8(3, e[0].ref[0]);
    // With no minimum age only the kept one stays.
    TEST_ASSERT_EQUAL(3, p3a_evict_choose(e, 4, 1000000, NOW, 0, &freed));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(e[i].keep);
    }
    TEST_ASSERT_TRUE(e[3].keep);
}

static void test_a_file_dated_before_the_clock_was_set_goes_first_and_one_from_the_future_stays(void)
{
    p3a_evict_entry_t e[] = {blob(1, 9 * HOUR, 100, false), blob(2, 0, 100, false), blob(3, 0, 100, false)};
    e[1].modified_at = 315532800;   // 1980: FAT's first day
    e[2].modified_at = NOW + 86400; // tomorrow
    TEST_ASSERT_EQUAL(2, p3a_evict_choose(e, 3, 1000000, NOW, P3A_EVICT_MIN_AGE_S, NULL));
    TEST_ASSERT_EQUAL_HEX8(2, e[0].ref[0]);
    TEST_ASSERT_EQUAL_HEX8(1, e[1].ref[0]);
    // A clock at the start of time, and a minimum age larger than it: nothing underflows, nothing goes.
    TEST_ASSERT_EQUAL(0, p3a_evict_choose(e, 3, 1000000, 100, P3A_EVICT_MIN_AGE_S, NULL));
}

static void test_only_the_stores_own_file_names_are_blobs(void)
{
    uint8_t ref[P3A_RECORD_BLOB_REF_LEN];
    p3a_format_t format = P3A_FORMAT_UNKNOWN;
    TEST_ASSERT_TRUE(p3a_evict_parse_name("00112233445566778899aabbccddeeff.webp", ref, &format));
    TEST_ASSERT_EQUAL(P3A_FORMAT_WEBP, format);
    TEST_ASSERT_EQUAL_HEX8(0x00, ref[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, ref[1]);
    TEST_ASSERT_EQUAL_HEX8(0xff, ref[15]);
    TEST_ASSERT_TRUE(p3a_evict_parse_name("00112233445566778899aabbccddeeff.jpg", ref, &format));
    TEST_ASSERT_EQUAL(P3A_FORMAT_JPEG, format);
    static const char *const not_blobs[] = {
        NULL,
        "",
        "00112233445566778899aabbccddeeff",            // no extension
        "00112233445566778899aabbccddeeff.txt",        // not a picture
        "00112233445566778899aabbccddeeff.webp.tmp",   // a file being written
        "00112233445566778899AABBCCDDEEFF.webp",       // the store writes lower case
        "00112233445566778899aabbccddeef.webp",        // a digit short
        "0011223344556677 899aabbccddeeff.webp",
        "00000000000000000000000000000000.webp",       // "no blob"
        "holiday photo of the family 2024.webp",
    };
    for (size_t i = 0; i < sizeof(not_blobs) / sizeof(not_blobs[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(p3a_evict_parse_name(not_blobs[i], ref, &format), not_blobs[i] ? not_blobs[i] : "NULL");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_marks_follow_the_card_within_their_limits);
    RUN_TEST(test_the_oldest_go_first_and_only_as_many_as_needed);
    RUN_TEST(test_young_and_kept_blobs_are_spared_whatever_is_needed);
    RUN_TEST(test_a_file_dated_before_the_clock_was_set_goes_first_and_one_from_the_future_stays);
    RUN_TEST(test_only_the_stores_own_file_names_are_blobs);
    return UNITY_END();
}
