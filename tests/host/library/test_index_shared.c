// SPDX-License-Identifier: Apache-2.0
// A channel's file as playsets of different sizes share it
// (p3a_index_shared.c): what is written, what is read back, and that two
// playsets taking turns lose neither the deeper listing nor what was
// downloaded.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "p3a_index.h"
#include "p3a_record.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define FILE_ROOM 64

static p3a_record_t record(unsigned n, bool blob)
{
    char id[16];
    snprintf(id, sizeof(id), "gif%03u", n);
    p3a_record_t r;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_giphy(&r, id));
    if (blob) {
        r.blob_ref[0] = (uint8_t)(n + 1);
        r.blob_ref[1] = 0xB0;
    }
    return r;
}

static const char *id_of(const p3a_record_t *r)
{
    return r->identity.giphy.id;
}

// An index of capacity `capacity` holding records first..first+count-1.
static p3a_index_t index_of(uint32_t capacity, unsigned first, unsigned count, bool blobs)
{
    p3a_index_t index;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&index, capacity));
    for (unsigned i = 0; i < count; i++) {
        const p3a_record_t r = record(first + i, blobs);
        TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&index, &r, NULL));
    }
    return index;
}

// What a playset of capacity `capacity` finds in a file: the index and what the file says.
static p3a_index_t load(const p3a_record_t *file, uint32_t count, uint32_t capacity, p3a_index_listing_t *listing)
{
    p3a_record_t copy[FILE_ROOM];
    memcpy(copy, file, count * sizeof(p3a_record_t));
    p3a_index_shared_read(copy, count, listing);
    p3a_index_t index;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&index, capacity));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_set_records(&index, copy, count < capacity ? count : capacity));
    return index;
}

static uint32_t save(const p3a_index_t *mine, const p3a_index_listing_t *listing, p3a_record_t *file, uint32_t count)
{
    p3a_record_t old[FILE_ROOM], out[FILE_ROOM];
    p3a_index_listing_t old_listing = {0};
    memcpy(old, file, count * sizeof(p3a_record_t));
    p3a_index_shared_read(old, count, &old_listing);
    const uint32_t n = p3a_index_shared_pack(mine, listing, count > 0 ? old : NULL, count, &old_listing, out);
    TEST_ASSERT_LESS_OR_EQUAL(FILE_ROOM, n);
    memcpy(file, out, n * sizeof(p3a_record_t));
    return n;
}

static void test_the_mark_is_a_flag_no_record_may_carry_in_memory(void)
{
    // p3a_record_check() refuses it: a record with the mark never gets into an index.
    TEST_ASSERT_EQUAL(0, P3A_RECORD_LISTING_END & P3A_RECORD_FLAGS_KNOWN);
    p3a_record_t r = record(1, false);
    r.flags |= P3A_RECORD_LISTING_END;
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_record_check(&r));
}

static void test_a_file_without_a_mark_is_one_whole_listing_that_ended_by_itself(void)
{
    p3a_record_t file[FILE_ROOM];
    for (unsigned i = 0; i < 5; i++) {
        file[i] = record(i, false);
    }
    p3a_index_listing_t listing;
    p3a_index_shared_read(file, 5, &listing);
    TEST_ASSERT_EQUAL(5, listing.listed);
    TEST_ASSERT_FALSE(listing.cut);
    p3a_index_shared_read(file, 0, &listing);
    TEST_ASSERT_EQUAL(0, listing.listed);
    TEST_ASSERT_FALSE(listing.cut);
}

static void test_a_listing_that_filled_its_index_is_marked_and_read_back(void)
{
    p3a_record_t file[FILE_ROOM];
    p3a_index_t mine = index_of(4, 0, 4, false);
    const p3a_index_listing_t listing = {.listed = 4, .cut = true};
    const uint32_t n = save(&mine, &listing, file, 0);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_BITS_HIGH(P3A_RECORD_LISTING_END, file[3].flags);
    TEST_ASSERT_BITS_LOW(P3A_RECORD_LISTING_END, file[0].flags);
    // The index itself was not touched.
    TEST_ASSERT_BITS_LOW(P3A_RECORD_LISTING_END, mine.records[3].flags);

    p3a_index_listing_t back;
    p3a_index_t again = load(file, n, 8, &back);
    TEST_ASSERT_EQUAL(4, back.listed);
    TEST_ASSERT_TRUE(back.cut);  // and a playset wanting 8 knows to ask the source again
    TEST_ASSERT_EQUAL(4, again.count);
    TEST_ASSERT_EQUAL(0, again.records[3].flags);
    p3a_index_destroy(&mine);
    p3a_index_destroy(&again);
}

static void test_a_listing_that_ended_by_itself_replaces_the_file(void)
{
    // The file held ten; the source now lists three and the index had room for more.
    p3a_record_t file[FILE_ROOM];
    p3a_index_t deep = index_of(10, 0, 10, true);
    uint32_t n = save(&deep, &(p3a_index_listing_t){.listed = 10, .cut = true}, file, 0);
    TEST_ASSERT_EQUAL(10, n);

    p3a_index_t mine = index_of(8, 20, 3, false);
    n = save(&mine, &(p3a_index_listing_t){.listed = 3, .cut = false}, file, n);
    TEST_ASSERT_EQUAL(3, n);
    p3a_index_listing_t back;
    p3a_index_shared_read(file, n, &back);
    TEST_ASSERT_FALSE(back.cut);
    TEST_ASSERT_EQUAL(3, back.listed);
    p3a_index_destroy(&deep);
    p3a_index_destroy(&mine);
}

static void test_a_shorter_playset_that_only_plays_keeps_the_deeper_listing_whole(void)
{
    // B listed ten (five downloaded). A wants four, downloads one more and saves, without listing.
    p3a_record_t file[FILE_ROOM];
    p3a_index_t deep = index_of(10, 0, 10, false);
    for (unsigned i = 0; i < 5; i++) {
        deep.records[i * 2].blob_ref[0] = 0x77;
    }
    uint32_t n = save(&deep, &(p3a_index_listing_t){.listed = 10, .cut = true}, file, 0);

    p3a_index_listing_t listing;
    p3a_index_t mine = load(file, n, 4, &listing);
    TEST_ASSERT_EQUAL(4, mine.count);
    TEST_ASSERT_EQUAL(10, listing.listed);  // the file's head, longer than what was loaded
    TEST_ASSERT_TRUE(listing.cut);
    mine.records[1].blob_ref[0] = 0x55;  // a download
    n = save(&mine, &listing, file, n);
    TEST_ASSERT_EQUAL(10, n);

    // B comes back and finds its ten, in order, with A's download, and no reason to list again.
    p3a_index_t back = load(file, n, 10, &listing);
    TEST_ASSERT_EQUAL(10, back.count);
    TEST_ASSERT_EQUAL(10, listing.listed);
    TEST_ASSERT_TRUE(listing.cut);
    for (unsigned i = 0; i < 10; i++) {
        const p3a_record_t want = record(i, false);
        TEST_ASSERT_EQUAL_STRING(id_of(&want), id_of(&back.records[i]));
    }
    TEST_ASSERT_EQUAL(0x55, back.records[1].blob_ref[0]);
    TEST_ASSERT_EQUAL(0x77, back.records[8].blob_ref[0]);
    TEST_ASSERT_FALSE(p3a_record_has_blob(&back.records[9]));  // kept though never downloaded: it is of the head
    p3a_index_destroy(&deep);
    p3a_index_destroy(&mine);
    p3a_index_destroy(&back);
}

static void test_a_shorter_listing_keeps_what_was_downloaded_beyond_it(void)
{
    // B listed ten; 0, 2, 4, 6, 8 are on the card. A (four) lists: the source now gives 20, 0, 6, 21.
    p3a_record_t file[FILE_ROOM];
    p3a_index_t deep = index_of(10, 0, 10, false);
    for (unsigned i = 0; i < 5; i++) {
        deep.records[i * 2].blob_ref[0] = (uint8_t)(0x70 + i);
    }
    uint32_t n = save(&deep, &(p3a_index_listing_t){.listed = 10, .cut = true}, file, 0);

    p3a_index_t fresh;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&fresh, 4));
    const unsigned now[] = {20, 0, 6, 21};
    for (unsigned i = 0; i < 4; i++) {
        const p3a_record_t r = record(now[i], false);
        TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&fresh, &r, NULL));
    }
    // What the whole file knows is brought over: 6 was beyond A's four.
    p3a_index_listing_t old_listing;
    p3a_index_t known = load(file, n, 10, &old_listing);
    p3a_index_carry_over(&fresh, &known, NULL);
    TEST_ASSERT_EQUAL(0x70, fresh.records[1].blob_ref[0]);
    TEST_ASSERT_EQUAL(0x73, fresh.records[2].blob_ref[0]);
    TEST_ASSERT_EQUAL(2, fresh.available_count);

    n = save(&fresh, &(p3a_index_listing_t){.listed = 4, .cut = true}, file, n);
    // The four, then 2, 4 and 8: downloaded, and not among the four. 1, 3, 5, 7, 9 had nothing to keep.
    TEST_ASSERT_EQUAL(7, n);
    p3a_index_listing_t back;
    p3a_index_t again = load(file, n, 10, &back);
    TEST_ASSERT_EQUAL(4, back.listed);
    TEST_ASSERT_TRUE(back.cut);  // B wants ten and the head is four: it lists again
    const unsigned want[] = {20, 0, 6, 21, 2, 4, 8};
    for (unsigned i = 0; i < 7; i++) {
        const p3a_record_t r = record(want[i], false);
        TEST_ASSERT_EQUAL_STRING(id_of(&r), id_of(&again.records[i]));
    }
    // Meanwhile B plays the four and the three it already had: five artworks on the card.
    TEST_ASSERT_EQUAL(5, again.available_count);
    p3a_index_destroy(&deep);
    p3a_index_destroy(&fresh);
    p3a_index_destroy(&known);
    p3a_index_destroy(&again);
}

static void test_the_file_never_grows_beyond_its_deepest_listing(void)
{
    // A channel always listed full at eight, whose artworks change entirely each time.
    p3a_record_t file[FILE_ROOM];
    uint32_t n = 0;
    for (unsigned round = 0; round < 6; round++) {
        p3a_index_t mine = index_of(8, round * 8, 8, true);
        n = save(&mine, &(p3a_index_listing_t){.listed = 8, .cut = true}, file, n);
        TEST_ASSERT_EQUAL(8, n);
        p3a_index_destroy(&mine);
    }
    // And one that was once deeper stays at that depth, however often the shorter one lists.
    p3a_index_t deep = index_of(16, 100, 16, true);
    n = save(&deep, &(p3a_index_listing_t){.listed = 16, .cut = true}, file, n);
    TEST_ASSERT_EQUAL(16, n);
    for (unsigned round = 0; round < 6; round++) {
        p3a_index_t mine = index_of(4, 200 + round * 4, 4, true);
        n = save(&mine, &(p3a_index_listing_t){.listed = 4, .cut = true}, file, n);
        TEST_ASSERT_EQUAL(16, n);
        p3a_index_listing_t back;
        p3a_record_t copy[FILE_ROOM];
        memcpy(copy, file, n * sizeof(p3a_record_t));
        p3a_index_shared_read(copy, n, &back);
        TEST_ASSERT_EQUAL(4, back.listed);
        p3a_index_destroy(&mine);
    }
    p3a_index_destroy(&deep);
}

static void test_an_empty_listing_writes_an_empty_file(void)
{
    p3a_record_t file[FILE_ROOM];
    p3a_index_t deep = index_of(4, 0, 4, true);
    uint32_t n = save(&deep, &(p3a_index_listing_t){.listed = 4, .cut = true}, file, 0);
    p3a_index_t none;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&none, 4));
    n = save(&none, &(p3a_index_listing_t){.listed = 0, .cut = false}, file, n);
    TEST_ASSERT_EQUAL(0, n);
    p3a_index_destroy(&deep);
    p3a_index_destroy(&none);
}

static void test_two_marks_in_a_file_do_not_confuse_the_reader(void)
{
    p3a_record_t file[FILE_ROOM];
    for (unsigned i = 0; i < 6; i++) {
        file[i] = record(i, true);
    }
    file[1].flags |= P3A_RECORD_LISTING_END;
    file[4].flags |= P3A_RECORD_LISTING_END;
    p3a_index_listing_t listing;
    p3a_index_shared_read(file, 6, &listing);
    TEST_ASSERT_EQUAL(2, listing.listed);
    TEST_ASSERT_TRUE(listing.cut);
    for (unsigned i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&file[i]));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_mark_is_a_flag_no_record_may_carry_in_memory);
    RUN_TEST(test_a_file_without_a_mark_is_one_whole_listing_that_ended_by_itself);
    RUN_TEST(test_a_listing_that_filled_its_index_is_marked_and_read_back);
    RUN_TEST(test_a_listing_that_ended_by_itself_replaces_the_file);
    RUN_TEST(test_a_shorter_playset_that_only_plays_keeps_the_deeper_listing_whole);
    RUN_TEST(test_a_shorter_listing_keeps_what_was_downloaded_beyond_it);
    RUN_TEST(test_the_file_never_grows_beyond_its_deepest_listing);
    RUN_TEST(test_an_empty_listing_writes_an_empty_file);
    RUN_TEST(test_two_marks_in_a_file_do_not_confuse_the_reader);
    return UNITY_END();
}
