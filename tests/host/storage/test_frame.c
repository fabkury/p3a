// SPDX-License-Identifier: Apache-2.0
// The CRC and the framed container.
#include "p3a_crc32.h"
#include "p3a_err.h"
#include "p3a_frame.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint8_t body[12];
} rec_t;

#define N 5
static rec_t s_recs[N];
static uint8_t s_file[sizeof(p3a_frame_header_t) + sizeof(s_recs) + 8];
static const p3a_frame_expect_t EXPECT = {
    .schema = P3A_FRAME_SCHEMA_TEST, .version = 3, .record_size = sizeof(rec_t), .max_count = 100};

// A well-formed file image of N records, as p3a_frame_save() lays it out.
static size_t build(uint32_t count)
{
    p3a_frame_header_t h;
    p3a_frame_header_make(&h, P3A_FRAME_SCHEMA_TEST, 3, sizeof(rec_t), count, P3A_FRAME_REBUILDABLE, s_recs);
    memcpy(s_file, &h, sizeof(h));
    memcpy(s_file + sizeof(h), s_recs, count * sizeof(rec_t));
    return sizeof(h) + count * sizeof(rec_t);
}

void setUp(void)
{
    for (uint32_t i = 0; i < N; i++) {
        s_recs[i].id = 1000 + i;
        memset(s_recs[i].body, (int)(i + 1), sizeof(s_recs[i].body));
    }
    memset(s_file, 0xEE, sizeof(s_file));
}
void tearDown(void) {}

// --- crc ------------------------------------------------------------------------

static void test_crc_known_values(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, p3a_crc32(0, "123456789", 9));  // the standard check value
    TEST_ASSERT_EQUAL_HEX32(0x00000000, p3a_crc32(0, "", 0));
    TEST_ASSERT_EQUAL_HEX32(0xE8B7BE43, p3a_crc32(0, "a", 1));
    TEST_ASSERT_EQUAL_HEX32(0x414FA339, p3a_crc32(0, "The quick brown fox jumps over the lazy dog", 43));
}

static void test_crc_continues_across_calls(void)
{
    const char *text = "The quick brown fox jumps over the lazy dog";
    uint32_t whole = p3a_crc32(0, text, 43);
    for (size_t split = 0; split <= 43; split++) {
        uint32_t crc = p3a_crc32(0, text, split);
        crc = p3a_crc32(crc, text + split, 43 - split);
        TEST_ASSERT_EQUAL_HEX32(whole, crc);
    }
}

// --- container ------------------------------------------------------------------

static void test_round_trip(void)
{
    size_t len = build(N);
    const void *records = NULL;
    uint32_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_check(s_file, len, &EXPECT, &records, &count));
    TEST_ASSERT_EQUAL_UINT32(N, count);
    TEST_ASSERT_EQUAL_PTR(s_file + 32, records);
    TEST_ASSERT_EQUAL_MEMORY(s_recs, records, sizeof(s_recs));
}

static void test_header_layout_on_disk(void)
{
    build(N);
    // The first bytes of every p3a container, byte for byte.
    const uint8_t expected_start[] = {'P', '3', 'A', 'F', 32, 0, 1, 0, 3, 0, 16, 0, N, 0, 0, 0, 1, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(expected_start, s_file, sizeof(expected_start));
}

static void test_empty_container_is_valid(void)
{
    size_t len = build(0);
    TEST_ASSERT_EQUAL_size_t(32, len);
    uint32_t count = 99;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_check(s_file, len, &EXPECT, NULL, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

static void test_trailing_bytes_are_ignored(void)
{
    size_t len = build(N);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_check(s_file, len + 8, &EXPECT, NULL, NULL));
}

static void test_truncation_is_a_size_error(void)
{
    size_t len = build(N);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_frame_check(s_file, 0, &EXPECT, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_frame_check(s_file, 31, &EXPECT, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_frame_check(s_file, 32, &EXPECT, NULL, NULL));       // header only
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_frame_check(s_file, len - 1, &EXPECT, NULL, NULL));  // one byte short
    // The header alone is still readable.
    size_t records_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_header_check(s_file, 32, &EXPECT, &records_len));
    TEST_ASSERT_EQUAL_size_t(sizeof(s_recs), records_len);
}

static void test_every_flipped_bit_is_caught(void)
{
    size_t len = build(N);
    for (size_t byte = 0; byte < len; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            s_file[byte] ^= (uint8_t)(1 << bit);
            esp_err_t err = p3a_frame_check(s_file, len, &EXPECT, NULL, NULL);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(ESP_OK, err, "a flipped bit went unnoticed");
            s_file[byte] ^= (uint8_t)(1 << bit);
        }
    }
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_check(s_file, len, &EXPECT, NULL, NULL));  // restored
}

static void test_damage_is_corrupt_not_version(void)
{
    size_t len = build(N);
    s_file[0] = 'X';  // magic
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_frame_check(s_file, len, &EXPECT, NULL, NULL));
    build(N);
    s_file[8] ^= 1;  // version byte, without fixing the header checksum
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_frame_check(s_file, len, &EXPECT, NULL, NULL));
    build(N);
    s_file[40] ^= 0x80;  // a record byte
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_frame_check(s_file, len, &EXPECT, NULL, NULL));
    // ... while the header of that file is still fine on its own.
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_header_check(s_file, len, &EXPECT, NULL));
}

static void test_another_firmwares_file_is_a_version_mismatch(void)
{
    size_t len = build(N);
    p3a_frame_expect_t e = EXPECT;
    e.version = 4;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, p3a_frame_check(s_file, len, &e, NULL, NULL));
    e = EXPECT;
    e.schema = 77;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, p3a_frame_check(s_file, len, &e, NULL, NULL));
    e = EXPECT;
    e.record_size = 192;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, p3a_frame_check(s_file, len, &e, NULL, NULL));
}

static void test_count_above_the_callers_limit_is_refused(void)
{
    size_t len = build(N);
    p3a_frame_expect_t e = EXPECT;
    e.max_count = N - 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_frame_check(s_file, len, &e, NULL, NULL));
    e.max_count = N;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_check(s_file, len, &e, NULL, NULL));
}

static void test_a_huge_claimed_count_cannot_overflow(void)
{
    // A header that is internally consistent but claims four billion records.
    p3a_frame_header_t h;
    p3a_frame_header_make(&h, P3A_FRAME_SCHEMA_TEST, 3, sizeof(rec_t), 0, 0, s_recs);
    h.count = 0xFFFFFFFFu;
    h.header_crc = p3a_crc32(0, &h, offsetof(p3a_frame_header_t, header_crc));
    memcpy(s_file, &h, sizeof(h));
    p3a_frame_expect_t e = EXPECT;
    e.max_count = 0xFFFFFFFFu;  // even a caller with no limit must get a clean error
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_frame_check(s_file, sizeof(s_file), &e, NULL, NULL));
}

static void test_unaligned_buffer(void)
{
    size_t len = build(N);
    uint8_t *shifted = malloc(len + 1);
    memcpy(shifted + 1, s_file, len);
    const void *records = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_frame_check(shifted + 1, len, &EXPECT, &records, NULL));
    TEST_ASSERT_EQUAL_PTR(shifted + 33, records);
    free(shifted);
}

static void test_bad_arguments(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_frame_check(NULL, 64, &EXPECT, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_frame_check(s_file, 64, NULL, NULL, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_known_values);
    RUN_TEST(test_crc_continues_across_calls);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_header_layout_on_disk);
    RUN_TEST(test_empty_container_is_valid);
    RUN_TEST(test_trailing_bytes_are_ignored);
    RUN_TEST(test_truncation_is_a_size_error);
    RUN_TEST(test_every_flipped_bit_is_caught);
    RUN_TEST(test_damage_is_corrupt_not_version);
    RUN_TEST(test_another_firmwares_file_is_a_version_mismatch);
    RUN_TEST(test_count_above_the_callers_limit_is_refused);
    RUN_TEST(test_a_huge_claimed_count_cannot_overflow);
    RUN_TEST(test_unaligned_buffer);
    RUN_TEST(test_bad_arguments);
    return UNITY_END();
}
