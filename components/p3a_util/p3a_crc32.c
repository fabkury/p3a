// SPDX-License-Identifier: Apache-2.0
#include "p3a_crc32.h"

// Half-byte table: 16 entries instead of 256. An index file of several
// hundred kilobytes still checks in a few milliseconds, and the table costs
// 64 bytes.
static const uint32_t s_table[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t p3a_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = s_table[crc & 0x0F] ^ (crc >> 4);
        crc = s_table[crc & 0x0F] ^ (crc >> 4);
    }
    return ~crc;
}
