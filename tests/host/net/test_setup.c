// SPDX-License-Identifier: Apache-2.0
// The pieces of setup that need no radio: the DNS catch-all, the lookup of
// built-in pages (against the real embedded table), and the rule for giving
// up on the stored network.
#include "http_static.h"
#include "net_backoff.h"
#include "net_dns.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const uint8_t IP[4] = {192, 168, 4, 1};
static uint8_t out[600];

// A query for one name. `name` is in wire form (length-prefixed labels, zero
// terminated). Returns its length.
static size_t make_query(uint8_t *q, const char *name, size_t name_len, unsigned qtype)
{
    static const uint8_t header[12] = {0xAB, 0xCD, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    memcpy(q, header, 12);
    memcpy(q + 12, name, name_len);
    size_t n = 12 + name_len;
    q[n++] = (uint8_t)(qtype >> 8);
    q[n++] = (uint8_t)qtype;
    q[n++] = 0x00;
    q[n++] = 0x01;
    return n;
}

#define EXAMPLE "\x07" "example" "\x03" "com"  // plus the terminating zero the literal adds

static void test_an_address_question_gets_this_device(void)
{
    uint8_t q[64];
    size_t len = make_query(q, EXAMPLE, sizeof(EXAMPLE), 1);
    size_t n = net_dns_answer(q, len, out, sizeof(out), IP);
    TEST_ASSERT_EQUAL(len + NET_DNS_ANSWER_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(0xAB, out[0]);  // the same id
    TEST_ASSERT_EQUAL_HEX8(0xCD, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x84, out[2]);  // a response, authoritative
    TEST_ASSERT_EQUAL_HEX8(0x00, out[3]);  // no error
    TEST_ASSERT_EQUAL(1, (out[4] << 8) | out[5]);
    TEST_ASSERT_EQUAL(1, (out[6] << 8) | out[7]);
    TEST_ASSERT_EQUAL_MEMORY(q + 12, out + 12, len - 12);  // the question, echoed
    static const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
    TEST_ASSERT_EQUAL_MEMORY(answer, out + len, sizeof(answer));
}

static void test_other_questions_get_an_empty_answer_at_once(void)
{
    uint8_t q[64];
    size_t len = make_query(q, EXAMPLE, sizeof(EXAMPLE), 28);  // an IPv6 address
    size_t n = net_dns_answer(q, len, out, sizeof(out), IP);
    TEST_ASSERT_EQUAL(len, n);
    TEST_ASSERT_EQUAL_HEX8(0x84, out[2]);
    TEST_ASSERT_EQUAL(0, (out[6] << 8) | out[7]);
    // "Any" is answered with the address.
    len = make_query(q, EXAMPLE, sizeof(EXAMPLE), 255);
    TEST_ASSERT_EQUAL(len + NET_DNS_ANSWER_BYTES, net_dns_answer(q, len, out, sizeof(out), IP));
}

static void test_what_follows_the_question_is_dropped(void)
{
    // A modern resolver appends an EDNS record; the answer must still come
    // straight after the question.
    uint8_t q[80];
    size_t len = make_query(q, EXAMPLE, sizeof(EXAMPLE), 1);
    q[11] = 1;  // one additional record
    static const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(q + len, opt, sizeof(opt));
    size_t n = net_dns_answer(q, len + sizeof(opt), out, sizeof(out), IP);
    TEST_ASSERT_EQUAL(len + NET_DNS_ANSWER_BYTES, n);
    TEST_ASSERT_EQUAL(0, out[11]);
    TEST_ASSERT_EQUAL_HEX8(0xC0, out[len]);
}

static void test_packets_that_are_not_plain_queries_are_ignored(void)
{
    uint8_t q[64];
    size_t len = make_query(q, EXAMPLE, sizeof(EXAMPLE), 1);
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, 11, out, sizeof(out), IP));   // shorter than a header
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, 16, out, sizeof(out), IP));   // cut inside the name
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len - 1, out, sizeof(out), IP));  // cut inside type and class
    q[2] = 0x81;  // a response
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len, out, sizeof(out), IP));
    q[2] = 0x29;  // an update, not a query
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len, out, sizeof(out), IP));
    q[2] = 0x01;
    q[5] = 0;     // no question
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len, out, sizeof(out), IP));
    q[5] = 1;
    q[12] = 0xC0;  // a compressed name in a question
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len, out, sizeof(out), IP));
    TEST_ASSERT_EQUAL(0, net_dns_answer(NULL, len, out, sizeof(out), IP));
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len, NULL, sizeof(out), IP));
}

static void test_a_label_that_runs_past_the_packet_is_ignored(void)
{
    uint8_t q[64];
    size_t len = make_query(q, EXAMPLE, sizeof(EXAMPLE), 1);
    q[12] = 63;  // the first label now claims more bytes than the packet has
    TEST_ASSERT_EQUAL(0, net_dns_answer(q, len, out, sizeof(out), IP));
}

// Every length of output buffer, and every truncation of every byte value in
// the length positions: the reply never passes the end of the buffer.
static void test_the_reply_never_overruns(void)
{
    uint8_t q[300];
    uint8_t name[256];
    // The longest legal name: labels of 63, 63, 63 and 61 bytes.
    size_t at = 0;
    static const uint8_t labels[] = {63, 63, 63, 61};
    for (size_t l = 0; l < sizeof(labels); l++) {
        name[at++] = labels[l];
        memset(name + at, 'a', labels[l]);
        at += labels[l];
    }
    name[at++] = 0;
    size_t len = make_query(q, (const char *)name, at, 1);
    for (size_t cap = 0; cap <= len + NET_DNS_ANSWER_BYTES + 4; cap++) {
        uint8_t guarded[400];
        memset(guarded, 0x5A, sizeof(guarded));
        size_t n = net_dns_answer(q, len, guarded, cap, IP);
        TEST_ASSERT_TRUE(n <= cap);
        TEST_ASSERT_EQUAL_HEX8(0x5A, guarded[cap]);
        if (cap >= len + NET_DNS_ANSWER_BYTES) {
            TEST_ASSERT_EQUAL(len + NET_DNS_ANSWER_BYTES, n);
        } else {
            TEST_ASSERT_EQUAL(0, n);
        }
    }
    // Garbage of every length is either ignored or answered within bounds.
    for (size_t glen = 0; glen < sizeof(q); glen++) {
        for (unsigned fill = 0; fill < 256; fill += 17) {
            uint8_t junk[300];
            memset(junk, (int)fill, sizeof(junk));
            junk[2] = 0x01;
            junk[5] = 1;
            size_t n = net_dns_answer(junk, glen, out, sizeof(out), IP);
            TEST_ASSERT_TRUE(n <= glen + NET_DNS_ANSWER_BYTES);
        }
    }
}

// --- built-in pages -------------------------------------------------------------

static const p3a_static_file_t *find(const char *path)
{
    return p3a_static_find(path, strlen(path));
}

static void test_the_setup_page_is_embedded_and_found(void)
{
    const p3a_static_file_t *page = find("/setup/index.html");
    TEST_ASSERT_NOT_NULL(page);
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", page->content_type);
    TEST_ASSERT_TRUE(page->gzipped);
    TEST_ASSERT_TRUE(page->len > 1000 && page->len < 6000);
    TEST_ASSERT_EQUAL_HEX8(0x1F, page->data[0]);  // the gzip signature
    TEST_ASSERT_EQUAL_HEX8(0x8B, page->data[1]);
    TEST_ASSERT_EQUAL('"', page->etag[0]);
    TEST_ASSERT_EQUAL(10, strlen(page->etag));
    // A folder path finds its index.
    TEST_ASSERT_TRUE(find("/setup/") == page);
}

static void test_static_lookup_is_exact(void)
{
    TEST_ASSERT_NULL(find("/setup"));
    // A page may be asked for without its ending; nothing else may.
    TEST_ASSERT_EQUAL_PTR(find("/setup/index.html"), find("/setup/index"));
    TEST_ASSERT_EQUAL_PTR(find("/index.html"), find("/index"));
    TEST_ASSERT_NULL(find("/static/common"));
    TEST_ASSERT_NULL(find("/setup/index."));
    TEST_ASSERT_NULL(find("/setup/index.htm"));
    TEST_ASSERT_NULL(find("/setup/index.html/"));
    TEST_ASSERT_NULL(find("/SETUP/index.html"));
    TEST_ASSERT_NULL(find("setup/index.html"));
    TEST_ASSERT_NULL(find("/setup/../setup/index.html"));
    TEST_ASSERT_EQUAL_PTR(find("/index.html"), find("/"));
    TEST_ASSERT_NOT_NULL(find("/"));
    TEST_ASSERT_NULL(find(""));
    TEST_ASSERT_NULL(p3a_static_find(NULL, 5));
    // The length is honoured, not the terminator.
    TEST_ASSERT_NOT_NULL(p3a_static_find("/setup/index.html?x=1", 17));
    TEST_ASSERT_NULL(p3a_static_find("/setup/index.html?x=1", 21));
}

// --- giving up on the stored network ---------------------------------------------

static void test_a_refusing_network_gets_three_tries(void)
{
    TEST_ASSERT_FALSE(net_first_join_gives_up(0, 0, false));
    TEST_ASSERT_FALSE(net_first_join_gives_up(2, 500000, false));  // time alone never ends this case
    TEST_ASSERT_TRUE(net_first_join_gives_up(3, 0, false));
    TEST_ASSERT_TRUE(net_first_join_gives_up(4, 0, false));
}

static void test_an_absent_network_gets_two_minutes(void)
{
    TEST_ASSERT_FALSE(net_first_join_gives_up(0, 0, true));
    TEST_ASSERT_FALSE(net_first_join_gives_up(0, NET_ABSENT_WINDOW_MS - 1, true));
    TEST_ASSERT_TRUE(net_first_join_gives_up(0, NET_ABSENT_WINDOW_MS, true));
    // The router came back and then refused us three times: setup, at once.
    TEST_ASSERT_TRUE(net_first_join_gives_up(3, 1000, true));
    // Long enough for a router to restart after a power cut.
    TEST_ASSERT_TRUE(NET_ABSENT_WINDOW_MS >= 90000);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_address_question_gets_this_device);
    RUN_TEST(test_other_questions_get_an_empty_answer_at_once);
    RUN_TEST(test_what_follows_the_question_is_dropped);
    RUN_TEST(test_packets_that_are_not_plain_queries_are_ignored);
    RUN_TEST(test_a_label_that_runs_past_the_packet_is_ignored);
    RUN_TEST(test_the_reply_never_overruns);
    RUN_TEST(test_the_setup_page_is_embedded_and_found);
    RUN_TEST(test_static_lookup_is_exact);
    RUN_TEST(test_a_refusing_network_gets_three_tries);
    RUN_TEST(test_an_absent_network_gets_two_minutes);
    return UNITY_END();
}
