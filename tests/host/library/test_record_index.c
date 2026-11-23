// SPDX-License-Identifier: Apache-2.0
// The record (its layout, how records are made, what is refused when one is
// read back) and the index (finding, order, availability, removal).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p3a_err.h"
#include "p3a_index.h"
#include "p3a_record.h"
#include "p3a_source.h"
#include "source_local.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// --- the record --------------------------------------------------------------

static void test_the_layout_is_the_one_on_disk(void)
{
    TEST_ASSERT_EQUAL(192, sizeof(p3a_record_t));
    TEST_ASSERT_EQUAL(0, offsetof(p3a_record_t, source));
    TEST_ASSERT_EQUAL(4, offsetof(p3a_record_t, id32));
    TEST_ASSERT_EQUAL(8, offsetof(p3a_record_t, sort_at));
    TEST_ASSERT_EQUAL(16, offsetof(p3a_record_t, width));
    TEST_ASSERT_EQUAL(22, offsetof(p3a_record_t, blob_ref));
    TEST_ASSERT_EQUAL(38, offsetof(p3a_record_t, reserved));
    TEST_ASSERT_EQUAL(64, offsetof(p3a_record_t, identity));
    TEST_ASSERT_EQUAL(66, offsetof(p3a_record_t, identity.museum.iiif_key));
    // Written to disk: these numbers never change.
    TEST_ASSERT_EQUAL(1, P3A_SOURCE_LOCAL);
    TEST_ASSERT_EQUAL(2, P3A_SOURCE_MAKAPIX);
    TEST_ASSERT_EQUAL(3, P3A_SOURCE_GIPHY);
    TEST_ASSERT_EQUAL(4, P3A_SOURCE_KLIPY);
    TEST_ASSERT_EQUAL(5, P3A_SOURCE_MUSEUM);
    TEST_ASSERT_EQUAL(4, P3A_FORMAT_JPEG);
}

static void test_each_source_makes_a_record_that_passes_its_own_check(void)
{
    const uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    p3a_record_t r;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_local(&r, "cat dancing.gif"));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    TEST_ASSERT_EQUAL_STRING("cat dancing.gif", r.identity.local.filename);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_makapix(&r, 123456, uuid));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    TEST_ASSERT_EQUAL(123456, r.id32);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_giphy(&r, "3o7TKsQ8UQ1h5bO3x2"));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_klipy(&r, 7654321987654321ull, 2));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_museum(&r, 7, "iiif/2/abc-def/full/843,/0/default.jpg"));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    TEST_ASSERT_EQUAL(P3A_SOURCE_MUSEUM, r.source);
    TEST_ASSERT_FALSE(p3a_record_has_blob(&r));
    r.blob_ref[15] = 1;
    TEST_ASSERT_TRUE(p3a_record_has_blob(&r));
}

static void test_an_identifier_that_does_not_fit_is_refused_not_cut(void)
{
    p3a_record_t r;
    char key[200];
    memset(key, 'k', sizeof(key));
    key[95] = '\0';  // 95 characters: the longest that fits
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_museum(&r, 1, key));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    key[95] = 'k';
    key[96] = '\0';  // 96: one too many
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_record_init_museum(&r, 1, key));
    key[31] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_giphy(&r, key));
    key[31] = 'k';
    key[32] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_record_init_giphy(&r, key));
    key[32] = 'k';
    key[96] = 'k';  // undo the cut made for the museum key
    key[119] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_local(&r, key));
    key[119] = 'k';
    key[120] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_record_init_local(&r, key));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_record_init_local(&r, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_record_init_giphy(&r, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_record_init_klipy(&r, 0, 1));
}

static void test_keys_differ_by_source_and_are_never_zero(void)
{
    p3a_record_t a, b, c;
    p3a_record_init_giphy(&a, "sameid");
    p3a_record_init_local(&b, "sameid");
    TEST_ASSERT_NOT_EQUAL(a.id32, b.id32);
    p3a_record_init_museum(&a, 1, "key");
    p3a_record_init_museum(&b, 2, "key");
    p3a_record_init_museum(&c, 1, "key");
    TEST_ASSERT_NOT_EQUAL(a.id32, b.id32);
    TEST_ASSERT_EQUAL(a.id32, c.id32);
    TEST_ASSERT_TRUE(p3a_record_same_identity(&a, &c));
    TEST_ASSERT_FALSE(p3a_record_same_identity(&a, &b));
    c.sort_at = 99;  // other fields do not make it another artwork
    c.blob_ref[0] = 1;
    TEST_ASSERT_TRUE(p3a_record_same_identity(&a, &c));
    for (uint32_t n = 0; n < 5000; n++) {
        TEST_ASSERT_NOT_EQUAL(0, p3a_record_hash(P3A_SOURCE_GIPHY, &n, sizeof(n)));
    }
}

static void test_a_damaged_record_is_refused(void)
{
    p3a_record_t good, r;
    p3a_record_init_giphy(&good, "abc123");
#define BREAK(statement)                                         \
    do {                                                         \
        r = good;                                                \
        statement;                                               \
        TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_record_check(&r)); \
    } while (0)
    BREAK(r.source = 0);
    BREAK(r.source = P3A_SOURCE_END_);
    BREAK(r.source = 0xFF);
    BREAK(r.kind = 1);
    BREAK(r.format = P3A_FORMAT_END_);
    BREAK(r.flags = 0x80);
    BREAK(r.id32 = 0);
    BREAK(r.reserved[25] = 1);
    BREAK(memset(r.identity.giphy.id, 'x', sizeof(r.identity.giphy.id)));  // no terminator
    BREAK(r.identity.giphy.id[0] = '\0');                                  // empty
    BREAK(r.identity.giphy.id[20] = 'z');                                  // text after the terminator
    BREAK(r.identity.raw[127] = 1);                                        // bytes this source does not use
#undef BREAK
    // The same bytes read as another source's identity must also hold up.
    r = good;
    r.source = P3A_SOURCE_KLIPY;  // nine bytes used; "abc123" spills nothing past them, id is nonzero
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    r.identity.raw[9] = 1;
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_record_check(&r));
}

// --- the index ---------------------------------------------------------------

static p3a_record_t numbered(uint32_t n, bool with_blob)
{
    p3a_record_t r;
    char name[32];
    snprintf(name, sizeof(name), "file-%05u.gif", (unsigned)n);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_local(&r, name));
    r.sort_at = n;
    if (with_blob) {
        memcpy(r.blob_ref, &n, sizeof(n));
        r.blob_ref[15] = 0xB1;
    }
    return r;
}

static void test_create_refuses_sizes_it_cannot_serve(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_index_create(&ix, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_index_create(&ix, P3A_INDEX_MAX_RECORDS + 1));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, 1));
    TEST_ASSERT_EQUAL(-1, p3a_index_find(&ix, 5));
    TEST_ASSERT_NULL(p3a_index_at(&ix, 0));
    TEST_ASSERT_EQUAL(-1, p3a_index_nth_available(&ix, 0));
    p3a_index_destroy(&ix);
    p3a_index_destroy(&ix);  // twice is harmless
}

static void test_a_full_index_finds_every_record_and_keeps_order(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, P3A_INDEX_MAX_RECORDS));
    for (uint32_t n = 0; n < P3A_INDEX_MAX_RECORDS; n++) {
        p3a_record_t r = numbered(n, n % 3 == 0);
        uint32_t at = 99999;
        TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&ix, &r, &at));
        TEST_ASSERT_EQUAL(n, at);
    }
    p3a_record_t extra = numbered(P3A_INDEX_MAX_RECORDS, false);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, p3a_index_append(&ix, &extra, NULL));
    TEST_ASSERT_EQUAL(P3A_INDEX_MAX_RECORDS, ix.count);
    TEST_ASSERT_EQUAL((P3A_INDEX_MAX_RECORDS + 2) / 3, ix.available_count);
    for (uint32_t n = 0; n < P3A_INDEX_MAX_RECORDS; n++) {
        p3a_record_t r = numbered(n, false);
        TEST_ASSERT_EQUAL((int32_t)n, p3a_index_find(&ix, r.id32));
        TEST_ASSERT_EQUAL((int32_t)n, p3a_index_find_record(&ix, &r));
        TEST_ASSERT_EQUAL(n, p3a_index_at(&ix, n)->sort_at);
        TEST_ASSERT_EQUAL(n % 3 == 0, p3a_index_is_available(&ix, n));
    }
    TEST_ASSERT_EQUAL(-1, p3a_index_find(&ix, extra.id32));
    // The n-th available record is record 3n.
    for (uint32_t n = 0; n < ix.available_count; n++) {
        TEST_ASSERT_EQUAL((int32_t)(n * 3), p3a_index_nth_available(&ix, n));
    }
    TEST_ASSERT_EQUAL(-1, p3a_index_nth_available(&ix, ix.available_count));
    p3a_index_destroy(&ix);
}

static void test_the_same_artwork_is_not_added_twice_but_a_shared_key_is_allowed(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, 8));
    p3a_record_t a = numbered(1, false);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&ix, &a, NULL));
    p3a_record_t again = a;
    again.sort_at = 777;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, p3a_index_append(&ix, &again, NULL));
    // Another artwork whose 32-bit key happens to be the same: both are kept,
    // and each is found by its identity.
    p3a_record_t twin = numbered(2, false);
    twin.id32 = a.id32;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&ix, &twin, NULL));
    TEST_ASSERT_EQUAL(0, p3a_index_find_record(&ix, &a));
    TEST_ASSERT_EQUAL(1, p3a_index_find_record(&ix, &twin));
    TEST_ASSERT_EQUAL(0, p3a_index_find(&ix, a.id32));
    // A record that would not pass a read is not let in either.
    p3a_record_t bad = numbered(3, false);
    bad.reserved[0] = 1;
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_index_append(&ix, &bad, NULL));
    TEST_ASSERT_EQUAL(2, ix.count);
    p3a_index_destroy(&ix);
}

static void test_removing_keeps_order_keys_and_availability_in_step(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, 64));
    for (uint32_t n = 0; n < 40; n++) {
        p3a_record_t r = numbered(n, n % 2 == 1);
        TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&ix, &r, NULL));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_index_remove(&ix, 40));
    // Remove 0 (unavailable), 9 (available), and the last.
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_remove(&ix, 39));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_remove(&ix, 9));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_remove(&ix, 0));
    TEST_ASSERT_EQUAL(37, ix.count);
    TEST_ASSERT_EQUAL(18, ix.available_count);
    uint32_t expect_at = 0, seen_available = 0;
    for (uint32_t n = 0; n < 40; n++) {
        p3a_record_t r = numbered(n, false);
        if (n == 0 || n == 9 || n == 39) {
            TEST_ASSERT_EQUAL(-1, p3a_index_find_record(&ix, &r));
            continue;
        }
        TEST_ASSERT_EQUAL((int32_t)expect_at, p3a_index_find_record(&ix, &r));
        TEST_ASSERT_EQUAL(n % 2 == 1, p3a_index_is_available(&ix, expect_at));
        if (n % 2 == 1) {
            TEST_ASSERT_EQUAL((int32_t)expect_at, p3a_index_nth_available(&ix, seen_available++));
        }
        expect_at++;
    }
    TEST_ASSERT_FALSE(p3a_index_is_available(&ix, 37));  // nothing lingers past the end
    p3a_index_destroy(&ix);
}

static void test_availability_follows_what_it_is_told(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, 16));
    for (uint32_t n = 0; n < 10; n++) {
        p3a_record_t r = numbered(n, false);
        p3a_index_append(&ix, &r, NULL);
    }
    TEST_ASSERT_EQUAL(0, ix.available_count);
    p3a_index_set_available(&ix, 4, true);
    p3a_index_set_available(&ix, 4, true);   // telling it twice counts once
    p3a_index_set_available(&ix, 8, true);
    p3a_index_set_available(&ix, 99, true);  // past the end: ignored
    TEST_ASSERT_EQUAL(2, ix.available_count);
    TEST_ASSERT_EQUAL(4, p3a_index_nth_available(&ix, 0));
    TEST_ASSERT_EQUAL(8, p3a_index_nth_available(&ix, 1));
    p3a_index_set_available(&ix, 4, false);
    p3a_index_set_available(&ix, 4, false);
    TEST_ASSERT_EQUAL(1, ix.available_count);
    TEST_ASSERT_EQUAL(8, p3a_index_nth_available(&ix, 0));
    p3a_index_clear(&ix);
    TEST_ASSERT_EQUAL(0, ix.count);
    TEST_ASSERT_EQUAL(0, ix.available_count);
    TEST_ASSERT_EQUAL(-1, p3a_index_nth_available(&ix, 0));
    p3a_index_destroy(&ix);
}

static void test_records_from_a_file_are_all_checked_before_any_is_taken(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, 32));
    p3a_record_t records[20];
    for (uint32_t n = 0; n < 20; n++) {
        records[n] = numbered(n, n < 5);
    }
    records[7].flags |= P3A_RECORD_GONE;
    memcpy(records[7].blob_ref, "has a blob too!!", 16);
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_set_records(&ix, records, 20));
    TEST_ASSERT_EQUAL(20, ix.count);
    TEST_ASSERT_EQUAL(5, ix.available_count);  // a record flagged gone is not offered, blob or not
    TEST_ASSERT_EQUAL(13, p3a_index_find_record(&ix, &records[13]));

    records[19].identity.raw[127] = 0x55;  // the last one is damaged
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_index_set_records(&ix, records, 20));
    TEST_ASSERT_EQUAL(0, ix.count);  // none of the twenty is taken
    TEST_ASSERT_EQUAL(-1, p3a_index_find_record(&ix, &records[3]));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, p3a_index_set_records(&ix, records, 33));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_set_records(&ix, NULL, 0));
    p3a_index_destroy(&ix);
}

// --- records in place, the source registry, local file names -------------------

static void test_a_record_in_place_is_available_without_a_blob(void)
{
    p3a_index_t ix;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&ix, 4));
    p3a_record_t r = numbered(1, false);
    r.flags = P3A_RECORD_IN_PLACE;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_check(&r));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&ix, &r, NULL));
    TEST_ASSERT_TRUE(p3a_index_is_available(&ix, 0));
    p3a_record_t gone = numbered(2, false);
    gone.flags = P3A_RECORD_IN_PLACE | P3A_RECORD_GONE;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_append(&ix, &gone, NULL));
    TEST_ASSERT_FALSE(p3a_index_is_available(&ix, 1));
    r.flags = 0x08;  // a flag this firmware does not know
    TEST_ASSERT_EQUAL(P3A_ERR_CORRUPT, p3a_record_check(&r));
    p3a_index_destroy(&ix);
}

static esp_err_t fake_refresh(const p3a_channel_ref_t *channel, p3a_index_t *index)
{
    (void)channel;
    (void)index;
    return ESP_OK;
}

static p3a_record_t giphy(const char *id)
{
    p3a_record_t r;
    TEST_ASSERT_EQUAL(ESP_OK, p3a_record_init_giphy(&r, id));
    return r;
}

static void test_a_fresh_listing_takes_over_what_was_known_about_its_artworks(void)
{
    p3a_index_t known = {0}, fresh = {0};
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&known, 8));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&fresh, 8));
    // Known: a (downloaded and measured), b (two failed downloads), c (downloaded).
    p3a_record_t a = giphy("a"), b = giphy("b"), c = giphy("c");
    memset(a.blob_ref, 0xA1, sizeof(a.blob_ref));
    a.format = P3A_FORMAT_GIF;
    a.width = 480;
    a.height = 270;
    b.download_fails = 2;
    memset(c.blob_ref, 0xC3, sizeof(c.blob_ref));
    c.format = P3A_FORMAT_WEBP;
    p3a_index_append(&known, &a, NULL);
    p3a_index_append(&known, &b, NULL);
    p3a_index_append(&known, &c, NULL);

    // The source now lists d (new), then a and b, in that order; c is gone from the channel.
    p3a_record_t d = giphy("d"), a2 = giphy("a"), b2 = giphy("b");
    p3a_index_append(&fresh, &d, NULL);
    p3a_index_append(&fresh, &a2, NULL);
    p3a_index_append(&fresh, &b2, NULL);
    TEST_ASSERT_EQUAL(0, fresh.available_count);

    p3a_index_merge_t stats;
    p3a_index_carry_over(&fresh, &known, &stats);
    TEST_ASSERT_EQUAL(2, stats.carried);
    TEST_ASSERT_EQUAL(1, stats.added);
    TEST_ASSERT_EQUAL(1, stats.dropped);
    TEST_ASSERT_EQUAL(3, fresh.count);
    // The source's order stands.
    TEST_ASSERT_EQUAL_STRING("d", p3a_index_at(&fresh, 0)->identity.giphy.id);
    const p3a_record_t *got_a = p3a_index_at(&fresh, 1), *got_b = p3a_index_at(&fresh, 2);
    TEST_ASSERT_EQUAL_HEX8(0xA1, got_a->blob_ref[0]);
    TEST_ASSERT_EQUAL(P3A_FORMAT_GIF, got_a->format);
    TEST_ASSERT_EQUAL(480, got_a->width);
    TEST_ASSERT_EQUAL(2, got_b->download_fails);
    TEST_ASSERT_FALSE(p3a_record_has_blob(got_b));
    // Only what has its bytes can be played.
    TEST_ASSERT_EQUAL(1, fresh.available_count);
    TEST_ASSERT_FALSE(p3a_index_is_available(&fresh, 0));
    TEST_ASSERT_TRUE(p3a_index_is_available(&fresh, 1));
    TEST_ASSERT_EQUAL(1, p3a_index_nth_available(&fresh, 0));
    // And the records are still found by key.
    TEST_ASSERT_EQUAL(2, p3a_index_find_record(&fresh, &b));

    // Nothing known before: everything is new, nothing playable. No index at all is the same.
    p3a_index_t empty = {0};
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&empty, 8));
    p3a_index_t again = {0};
    TEST_ASSERT_EQUAL(ESP_OK, p3a_index_create(&again, 8));
    p3a_index_append(&again, &d, NULL);
    p3a_index_carry_over(&again, &empty, &stats);
    TEST_ASSERT_EQUAL(1, stats.added);
    TEST_ASSERT_EQUAL(0, stats.dropped);
    p3a_index_carry_over(&again, NULL, &stats);
    TEST_ASSERT_EQUAL(1, stats.added);
    p3a_index_carry_over(&again, &known, NULL);  // no statistics wanted

    // An artwork the source flags as gone is not playable though its bytes are still here.
    p3a_index_clear(&fresh);
    a2.flags |= P3A_RECORD_GONE;
    p3a_index_append(&fresh, &a2, NULL);
    p3a_index_carry_over(&fresh, &known, &stats);
    TEST_ASSERT_TRUE(p3a_record_has_blob(p3a_index_at(&fresh, 0)));
    TEST_ASSERT_EQUAL(0, fresh.available_count);

    p3a_index_destroy(&known);
    p3a_index_destroy(&fresh);
    p3a_index_destroy(&empty);
    p3a_index_destroy(&again);
}

static void test_sources_register_once_and_unknown_ones_are_absent(void)
{
    static const p3a_source_ops_t giphy = {.source = P3A_SOURCE_GIPHY, .name = "giphy", .refresh = fake_refresh};
    static const p3a_source_ops_t pinned = {.source = P3A_CHANNEL_SOURCE_PINNED, .name = "pinned", .refresh = fake_refresh};
    static const p3a_source_ops_t no_refresh = {.source = P3A_SOURCE_KLIPY, .name = "klipy"};
    static const p3a_source_ops_t out_of_range = {.source = P3A_CHANNEL_SOURCE_END_, .name = "x", .refresh = fake_refresh};
    static const p3a_source_ops_t zero = {.source = 0, .name = "x", .refresh = fake_refresh};
    p3a_source_reset_for_test();
    TEST_ASSERT_NULL(p3a_source_get(P3A_SOURCE_GIPHY));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_source_register(&giphy));
    TEST_ASSERT_EQUAL(ESP_OK, p3a_source_register(&pinned));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, p3a_source_register(&giphy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_source_register(&no_refresh));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_source_register(&out_of_range));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_source_register(&zero));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, p3a_source_register(NULL));
    TEST_ASSERT_EQUAL_PTR(&giphy, p3a_source_get(P3A_SOURCE_GIPHY));
    TEST_ASSERT_EQUAL_PTR(&pinned, p3a_source_get(6));
    TEST_ASSERT_NULL(p3a_source_get(P3A_SOURCE_KLIPY));
    TEST_ASSERT_NULL(p3a_source_get(0));
    TEST_ASSERT_NULL(p3a_source_get(250));
}

static void test_local_files_are_pictures_by_their_extension(void)
{
    TEST_ASSERT_EQUAL(P3A_FORMAT_GIF, source_local_format_of("cat.gif"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_GIF, source_local_format_of("CAT.GIF"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_JPEG, source_local_format_of("a photo.JPeg"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_JPEG, source_local_format_of("x.jpg"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_PNG, source_local_format_of("anim.apng"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_WEBP, source_local_format_of("archive.tar.webp"));
    TEST_ASSERT_EQUAL(P3A_FORMAT_BMP, source_local_format_of("b.bmp"));
    const char *passed_over[] = {"notes.txt", "gif", ".gif", ".hidden.png", "._cat.gif", "cat.gif.tmp",
                                 "cat.gif.bak", "cat.", "cat", "", "cat.gifx"};
    for (size_t i = 0; i < sizeof(passed_over) / sizeof(passed_over[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(P3A_FORMAT_UNKNOWN, source_local_format_of(passed_over[i]), passed_over[i]);
    }
    TEST_ASSERT_EQUAL(P3A_FORMAT_UNKNOWN, source_local_format_of(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_record_in_place_is_available_without_a_blob);
    RUN_TEST(test_a_fresh_listing_takes_over_what_was_known_about_its_artworks);
    RUN_TEST(test_sources_register_once_and_unknown_ones_are_absent);
    RUN_TEST(test_local_files_are_pictures_by_their_extension);
    RUN_TEST(test_the_layout_is_the_one_on_disk);
    RUN_TEST(test_each_source_makes_a_record_that_passes_its_own_check);
    RUN_TEST(test_an_identifier_that_does_not_fit_is_refused_not_cut);
    RUN_TEST(test_keys_differ_by_source_and_are_never_zero);
    RUN_TEST(test_a_damaged_record_is_refused);
    RUN_TEST(test_create_refuses_sizes_it_cannot_serve);
    RUN_TEST(test_a_full_index_finds_every_record_and_keeps_order);
    RUN_TEST(test_the_same_artwork_is_not_added_twice_but_a_shared_key_is_allowed);
    RUN_TEST(test_removing_keeps_order_keys_and_availability_in_step);
    RUN_TEST(test_availability_follows_what_it_is_told);
    RUN_TEST(test_records_from_a_file_are_all_checked_before_any_is_taken);
    return UNITY_END();
}
