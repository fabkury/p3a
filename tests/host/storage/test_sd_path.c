// SPDX-License-Identifier: Apache-2.0
#include "sd_path.h"
#include "unity.h"

#include <string.h>

// Expected paths, from an independent implementation of the layout in Python
// (the same one tests/device uses against the real device).
#define SHARD_ABC "/b/61/5/abc.webp"
#define SHARD_UUID "/b/44/51/3f2b8c1e-7d4a-4e0b-9c1d-5a6b7c8d9e0f.gif"
#define SHARD_HAM "/b/25/24/urn-3_HUAM_12345_dynmc.jpg"

static char buf[256];

void setUp(void)
{
    sd_path_init(NULL);
    memset(buf, 'x', sizeof(buf));
}
void tearDown(void) {}

// --- the root -------------------------------------------------------------------

static void test_default_root(void)
{
    TEST_ASSERT_EQUAL_STRING("/sdcard/p3a", sd_path_get_root());
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_init(""));
    TEST_ASSERT_EQUAL_STRING("/sdcard/p3a", sd_path_get_root());
}

static void test_user_form_gets_the_mount_point(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_init("/art"));
    TEST_ASSERT_EQUAL_STRING("/sdcard/art", sd_path_get_root());
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_init("/a/b/c"));
    TEST_ASSERT_EQUAL_STRING("/sdcard/a/b/c", sd_path_get_root());
}

static void test_full_form_is_used_as_is(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_init("/sdcard/art"));
    TEST_ASSERT_EQUAL_STRING("/sdcard/art", sd_path_get_root());
    // A folder merely starting with "sdcard" is an ordinary user folder.
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_init("/sdcardx"));
    TEST_ASSERT_EQUAL_STRING("/sdcard/sdcardx", sd_path_get_root());
}

static void test_invalid_roots_are_rejected(void)
{
    static const char *const bad[] = {"", "art", "/", "/../etc", "/a/../b", "/sdcard/", "relative/path", ".."};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, sd_path_validate_root(bad[i]), bad[i]);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_validate_root(NULL));
}

static void test_an_invalid_stored_root_falls_back_to_the_default_and_says_so(void)
{
    sd_path_init("/good");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_init("/a/../b"));
    TEST_ASSERT_EQUAL_STRING("/sdcard/p3a", sd_path_get_root());
}

static void test_length_limit_applies_to_the_resolved_root(void)
{
    char root[80];
    // User form: 7 characters of mount point are added, and 63 is the longest resolved root.
    memset(root, 'a', sizeof(root));
    root[0] = '/';
    root[56] = '\0';  // 56 + 7 = 63
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_validate_root(root));
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_init(root));
    TEST_ASSERT_EQUAL_size_t(63, strlen(sd_path_get_root()));
    root[56] = 'a';
    root[57] = '\0';  // 57 + 7 = 64: one too many
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sd_path_validate_root(root));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sd_path_init(root));
    TEST_ASSERT_EQUAL_STRING("/sdcard/p3a", sd_path_get_root());
}

static void test_subdirs_follow_the_root(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_get_animations(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/p3a/animations", buf);
    sd_path_init("/art");
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_get_temporary(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/art/temporary", buf);
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_get_subdir("blobs", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/art/blobs", buf);
}

static void test_subdir_buffer_too_small(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sd_path_get_subdir("animations", buf, 22));  // needs 23
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_get_subdir("animations", buf, 23));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_get_subdir(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_get_subdir("x", buf, 0));
}

// --- names ----------------------------------------------------------------------

static void test_sanitize_replaces_exactly_the_fat_reserved_characters(void)
{
    sd_path_sanitize_filename("a:b/c\\d?e*f\"g<h>i|j", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("a_b_c_d_e_f_g_h_i_j", buf);
    sd_path_sanitize_filename("plain-name_1.2 (3)", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("plain-name_1.2 (3)", buf);
    sd_path_sanitize_filename("urn-3:HUAM:12345_dynmc", buf, sizeof(buf));  // a Harvard key
    TEST_ASSERT_EQUAL_STRING("urn-3_HUAM_12345_dynmc", buf);
}

static void test_sanitize_is_idempotent_and_in_place(void)
{
    strcpy(buf, "x:y?z");
    sd_path_sanitize_filename(buf, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("x_y_z", buf);
    sd_path_sanitize_filename(buf, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("x_y_z", buf);
}

static void test_sanitize_truncates_and_handles_null(void)
{
    sd_path_sanitize_filename("abcdef", buf, 4);
    TEST_ASSERT_EQUAL_STRING("abc", buf);
    sd_path_sanitize_filename(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    sd_path_sanitize_filename("abc", NULL, 0);  // must not crash
}

// --- the shard layout -----------------------------------------------------------

static void test_sharded_path_values_are_the_on_disk_format(void)
{
    // Computed independently (FNV-1a 64 over the sanitized name, then fmix64,
    // low 6 bits of bytes 0 and 1). If one of these changes, every stored file
    // has moved: that is an on-disk format break, not a refactor.
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_build_sharded("/b", "abc", ".webp", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(SHARD_ABC, buf);
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_build_sharded("/b", "3f2b8c1e-7d4a-4e0b-9c1d-5a6b7c8d9e0f", ".gif", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(SHARD_UUID, buf);
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_build_sharded("/b", "urn-3:HUAM:12345_dynmc", ".jpg", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(SHARD_HAM, buf);
}

static void test_raw_and_sanitized_names_land_in_the_same_place(void)
{
    char raw[128], clean[128];
    sd_path_build_sharded("/b", "urn-3:HUAM:12345_dynmc", ".jpg", raw, sizeof(raw));
    sd_path_build_sharded("/b", "urn-3_HUAM_12345_dynmc", ".jpg", clean, sizeof(clean));
    TEST_ASSERT_EQUAL_STRING(clean, raw);
}

static void test_extension_does_not_move_the_file(void)
{
    char a[128], b[128];
    sd_path_build_sharded("/b", "same-name", ".gif", a, sizeof(a));
    sd_path_build_sharded("/b", "same-name", ".webp", b, sizeof(b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, strlen(a) - 4);  // identical up to the extension
    sd_path_build_sharded("/b", "same-name", NULL, a, sizeof(a));
    TEST_ASSERT_TRUE(strstr(a, "/same-name") != NULL && a[strlen(a) - 1] == 'e');
}

static void test_shard_folders_are_within_range_and_spread(void)
{
    int used[64] = {0};
    for (int i = 0; i < 2000; i++) {
        char name[24];
        snprintf(name, sizeof(name), "artwork-%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, sd_path_build_sharded("", name, "", buf, sizeof(buf)));
        int d0 = -1, d1 = -1;
        TEST_ASSERT_EQUAL_INT(2, sscanf(buf, "/%d/%d/", &d0, &d1));
        TEST_ASSERT_TRUE(d0 >= 0 && d0 < 64 && d1 >= 0 && d1 < 64);
        used[d0]++;
    }
    for (int d = 0; d < 64; d++) {
        // 2000 names over 64 folders is about 31 each; an empty or crowded one means a bad hash.
        TEST_ASSERT_TRUE_MESSAGE(used[d] > 8 && used[d] < 70, "first-level folders are unevenly used");
    }
}

static void test_sharded_buffer_limits(void)
{
    sd_path_build_sharded("/b", "abc", ".webp", buf, sizeof(buf));
    size_t need = strlen(buf) + 1;
    char small[64];
    TEST_ASSERT_EQUAL(ESP_OK, sd_path_build_sharded("/b", "abc", ".webp", small, need));
    for (size_t n = 1; n < need; n++) {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sd_path_build_sharded("/b", "abc", ".webp", small, n));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_build_sharded("/b", "", ".webp", small, sizeof(small)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_build_sharded(NULL, "abc", ".webp", small, sizeof(small)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_path_build_sharded("/b", "abc", ".webp", small, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_root);
    RUN_TEST(test_user_form_gets_the_mount_point);
    RUN_TEST(test_full_form_is_used_as_is);
    RUN_TEST(test_invalid_roots_are_rejected);
    RUN_TEST(test_an_invalid_stored_root_falls_back_to_the_default_and_says_so);
    RUN_TEST(test_length_limit_applies_to_the_resolved_root);
    RUN_TEST(test_subdirs_follow_the_root);
    RUN_TEST(test_subdir_buffer_too_small);
    RUN_TEST(test_sanitize_replaces_exactly_the_fat_reserved_characters);
    RUN_TEST(test_sanitize_is_idempotent_and_in_place);
    RUN_TEST(test_sanitize_truncates_and_handles_null);
    RUN_TEST(test_sharded_path_values_are_the_on_disk_format);
    RUN_TEST(test_raw_and_sanitized_names_land_in_the_same_place);
    RUN_TEST(test_extension_does_not_move_the_file);
    RUN_TEST(test_shard_folders_are_within_range_and_spread);
    RUN_TEST(test_sharded_buffer_limits);
    return UNITY_END();
}
