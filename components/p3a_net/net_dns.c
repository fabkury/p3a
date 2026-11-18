// SPDX-License-Identifier: Apache-2.0
// See net_dns.h. No IDF includes: also compiled by the host tests.

#include "net_dns.h"

#include <string.h>

#define DNS_HEADER_BYTES 12

size_t net_dns_answer(const uint8_t *query, size_t query_len, uint8_t *out, size_t out_len, const uint8_t ip[4])
{
    if (query == NULL || out == NULL || ip == NULL || query_len < DNS_HEADER_BYTES + 5) {
        return 0;  // a header and the smallest question: root name, type, class
    }
    // Anyone on the open access point can send anything here. Answer only a
    // plain query that carries a question, and only if the reply fits.
    if ((query[2] & 0x80) != 0 || (query[2] & 0x78) != 0) {
        return 0;  // a response, or not a standard query
    }
    if (query[4] == 0 && query[5] == 0) {
        return 0;  // no question
    }
    if (query_len > out_len || out_len - query_len < NET_DNS_ANSWER_BYTES) {
        return 0;
    }
    // The first question's name must end inside the packet, or the pointer in
    // the answer would point at nothing.
    size_t at = DNS_HEADER_BYTES;
    while (at < query_len && query[at] != 0) {
        if ((query[at] & 0xC0) != 0) {
            return 0;  // compression in a question: not something a client sends
        }
        at += (size_t)query[at] + 1;
    }
    if (at + 5 > query_len) {
        return 0;  // no room left for the terminator, type and class
    }

    // The reply is the header and the first question, then the answer. What
    // followed the question in the query (a second question, an EDNS record)
    // is dropped, so that the answer sits where a resolver expects it.
    size_t n = at + 5;
    memcpy(out, query, n);
    out[2] = 0x84;  // response, authoritative
    out[3] = 0x00;  // no error
    out[4] = 0x00;  // one question
    out[5] = 0x01;
    out[6] = 0x00;  // one answer
    out[7] = 0x01;
    out[8] = out[9] = out[10] = out[11] = 0;  // no authority or additional records
    // Only a question for an IPv4 address gets one. Anything else (an IPv6
    // address, mostly) is answered "no such record", which makes the client
    // fall back to IPv4 at once instead of waiting for a timeout.
    unsigned qtype = ((unsigned)query[at + 1] << 8) | query[at + 2];
    if (qtype != 1 && qtype != 255) {
        out[7] = 0x00;
        return n;
    }
    out[n++] = 0xC0;  // the name: a pointer to the question's
    out[n++] = DNS_HEADER_BYTES;
    out[n++] = 0x00;  // type A
    out[n++] = 0x01;
    out[n++] = 0x00;  // class IN
    out[n++] = 0x01;
    out[n++] = 0x00;  // time to live: 60 s
    out[n++] = 0x00;
    out[n++] = 0x00;
    out[n++] = 0x3C;
    out[n++] = 0x00;  // four bytes of address
    out[n++] = 0x04;
    memcpy(out + n, ip, 4);
    return n + 4;
}
