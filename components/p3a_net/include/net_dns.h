// SPDX-License-Identifier: Apache-2.0
// The DNS answer the setup access point gives: every name is this device.
//
// While a phone is joined to the device's own access point there is no
// internet behind it. Answering every DNS question with the device's address
// sends whatever the phone tries to open, and in particular the operating
// system's "is this a captive portal" probe, to the setup page.
//
// Host-testable.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The answer adds this many bytes to the question.
#define NET_DNS_ANSWER_BYTES 16

// Builds the reply to one DNS query. A question for an IPv4 address is
// answered with `ip`; any other question is answered with no records. `ip` is the address to give, as four
// bytes in the order they are written (192, 168, 4, 1). Returns the reply
// length, or 0 when the packet should be ignored: too short to be a query,
// not a standard query, a response, carrying no question, or too long for
// the reply to fit in out_len.
size_t net_dns_answer(const uint8_t *query, size_t query_len, uint8_t *out, size_t out_len, const uint8_t ip[4]);

#ifdef __cplusplus
}
#endif
