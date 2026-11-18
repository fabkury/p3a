// SPDX-License-Identifier: Apache-2.0
// CRC-32 (IEEE 802.3, the one zlib and PNG use). The same code runs on the
// device and in the host tests, so a file written by one is checked the same
// way by the other.
//
// Host-testable: no IDF includes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Continues a CRC over more data. Start with crc = 0:
//   crc = p3a_crc32(0, a, a_len);
//   crc = p3a_crc32(crc, b, b_len);   // equals the CRC of a followed by b
uint32_t p3a_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
