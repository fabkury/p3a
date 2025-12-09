/**
 * @file pie_memcpy_128.h
 * @brief PIE SIMD memcpy for ESP32-P4
 * 
 * Fast memory copy using ESP32-P4 PIE 128-bit vector instructions.
 * Used for duplicating rows during vertical upscaling.
 */

#ifndef P3A_PIE_MEMCPY_128_H
#define P3A_PIE_MEMCPY_128_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copy memory using PIE 128-bit SIMD instructions
 * 
 * Efficiently copies memory using ESP32-P4 PIE vector load/store.
 * Handles arbitrary byte counts including non-multiples of 128 and 16.
 * 
 * For best performance:
 * - Keep dst and src 16-byte aligned
 * - Use len that is a multiple of 16
 * 
 * @param dst Destination pointer
 * @param src Source pointer  
 * @param len Number of bytes to copy
 */
void pie_memcpy_128(void *dst, const void *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif // P3A_PIE_MEMCPY_128_H
