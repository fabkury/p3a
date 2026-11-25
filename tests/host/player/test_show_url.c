// SPDX-License-Identifier: Apache-2.0
// "Show this": which addresses may be fetched, and the name a picture gets.
#include <string.h>

#include "show_url.h"
#include "source_local.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_only_web_addresses_with_a_host_are_fetched(void)
{
    TEST_ASSERT_TRUE(show_url_ok("https://example.org/a.gif"));
    TEST_ASSERT_TRUE(show_url_ok("http://192.168.1.20:8080/pictures/a.gif?size=2"));
    TEST_ASSERT_TRUE(show_url_ok("https://example.org"));
    TEST_ASSERT_FALSE(show_url_ok(NULL));
    TEST_ASSERT_FALSE(show_url_ok(""));
    TEST_ASSERT_FALSE(show_url_ok("example.org/a.gif"));
    TEST_ASSERT_FALSE(show_url_ok("ftp://example.org/a.gif"));
    TEST_ASSERT_FALSE(show_url_ok("file:///sdcard/a.gif"));
    TEST_ASSERT_FALSE(show_url_ok("HTTPS://example.org/a.gif"));
    TEST_ASSERT_FALSE(show_url_ok("https://"));
    TEST_ASSERT_FALSE(show_url_ok("https:///a.gif"));
    TEST_ASSERT_FALSE(show_url_ok("https://?a.gif"));
    TEST_ASSERT_FALSE(show_url_ok("https://example.org/a b.gif"));
    TEST_ASSERT_FALSE(show_url_ok("https://example.org/a.gif\r\nHost: other"));
    static char long_url[SHOW_URL_MAX_LEN + 8];
    memset(long_url, 'a', sizeof(long_url) - 1);
    memcpy(long_url, "https://", 8);
    TEST_ASSERT_FALSE(show_url_ok(long_url));
    long_url[SHOW_URL_MAX_LEN - 1] = '\0';
    TEST_ASSERT_TRUE(show_url_ok(long_url));
}

static const char *name_of(const char *url, p3a_format_t format)
{
    static char name[P3A_RECORD_FILENAME_LEN];
    memset(name, 'X', sizeof(name));
    TEST_ASSERT_EQUAL(ESP_OK, show_url_name(url, format, name, sizeof(name)));
    TEST_ASSERT_TRUE(strlen(name) < sizeof(name));
    // Whatever the address, the local source will take the file for what it is.
    TEST_ASSERT_EQUAL_MESSAGE(format, source_local_format_of(name), name);
    TEST_ASSERT_NULL_MESSAGE(strpbrk(name, ":/\\?*\"<>| %"), name);
    return name;
}

static void test_the_name_is_the_last_segment_and_the_bytes_decide_the_extension(void)
{
    TEST_ASSERT_EQUAL_STRING("cat.gif", name_of("https://example.org/pictures/cat.gif", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("cat.gif", name_of("https://example.org/pictures/cat.gif?size=2#top", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("cat.GIF", name_of("https://example.org/cat.GIF", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("cat.jpeg", name_of("https://example.org/cat.jpeg", P3A_FORMAT_JPEG));
    TEST_ASSERT_EQUAL_STRING("cat.apng", name_of("https://example.org/cat.apng", P3A_FORMAT_PNG));
    // No extension, or another format's: what the bytes are is added.
    TEST_ASSERT_EQUAL_STRING("default.jpg", name_of("https://iiif.example/abc/full/!720,720/0/default", P3A_FORMAT_JPEG));
    TEST_ASSERT_EQUAL_STRING("cat.gif.webp", name_of("https://example.org/cat.gif", P3A_FORMAT_WEBP));
    TEST_ASSERT_EQUAL_STRING("download.php.png", name_of("https://example.org/download.php?id=7", P3A_FORMAT_PNG));
    // A trailing slash names the folder.
    TEST_ASSERT_EQUAL_STRING("pictures.bmp", name_of("https://example.org/pictures/", P3A_FORMAT_BMP));
    // No path at all, or nothing usable in it.
    TEST_ASSERT_EQUAL_STRING("shown.gif", name_of("https://example.org", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("shown.gif", name_of("https://example.org/", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("shown.gif", name_of("https://example.org/?x=cat.png", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("shown.gif", name_of("https://example.org/%2F%2F", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("shown.gif", name_of("https://example.org/...", P3A_FORMAT_GIF));
}

static void test_a_name_is_made_safe_for_the_card(void)
{
    // Escapes are resolved first, so an escaped separator cannot name another folder.
    TEST_ASSERT_EQUAL_STRING("my_cat.gif", name_of("https://example.org/my%20cat.gif", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("___.._settings.gif", name_of("https://example.org/%2e%2e%2F..%5Csettings", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("a_b_c_d.gif", name_of("https://example.org/a:b*c|d.gif", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("caf__.gif", name_of("https://example.org/caf%C3%A9.gif", P3A_FORMAT_GIF));
    // A broken escape is kept as text, made safe.
    TEST_ASSERT_EQUAL_STRING("100_.gif", name_of("https://example.org/100%.gif", P3A_FORMAT_GIF));
    TEST_ASSERT_EQUAL_STRING("a_zz.gif", name_of("https://example.org/a%zz.gif", P3A_FORMAT_GIF));
    // A hidden name would be passed over by the local source.
    TEST_ASSERT_EQUAL_STRING("_hidden.gif", name_of("https://example.org/.hidden.gif", P3A_FORMAT_GIF));
    // A long segment is cut to what a record's name holds, and still ends as a picture.
    static char url[600];
    memset(url, 'n', sizeof(url) - 1);
    memcpy(url, "https://example.org/", 20);
    memcpy(url + sizeof(url) - 5, ".gif", 5);
    const char *name = name_of(url, P3A_FORMAT_GIF);
    TEST_ASSERT_EQUAL(P3A_RECORD_FILENAME_LEN - 1, strlen(name));
    TEST_ASSERT_EQUAL_STRING(".gif", name + strlen(name) - 4);
}

static void test_what_cannot_be_named_is_refused(void)
{
    char name[P3A_RECORD_FILENAME_LEN], tiny[8];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, show_url_name("https://example.org/a.gif", P3A_FORMAT_UNKNOWN, name, sizeof(name)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, show_url_name(NULL, P3A_FORMAT_GIF, name, sizeof(name)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, show_url_name("https://example.org/a.gif", P3A_FORMAT_GIF, tiny, sizeof(tiny)));
    // A buffer smaller than a record's name is filled, never overrun.
    char small[16];
    memset(small, 'X', sizeof(small));
    TEST_ASSERT_EQUAL(ESP_OK, show_url_name("https://example.org/a-very-long-name-indeed.gif", P3A_FORMAT_GIF, small, 12));
    TEST_ASSERT_EQUAL(11, strlen(small));
    TEST_ASSERT_EQUAL_STRING("a-very-.gif", small);
    TEST_ASSERT_EQUAL('X', small[12]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_only_web_addresses_with_a_host_are_fetched);
    RUN_TEST(test_the_name_is_the_last_segment_and_the_bytes_decide_the_extension);
    RUN_TEST(test_a_name_is_made_safe_for_the_card);
    RUN_TEST(test_what_cannot_be_named_is_refused);
    return UNITY_END();
}
