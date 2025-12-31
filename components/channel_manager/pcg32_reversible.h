// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/*
 * pcg32_reversible.h
 *
 * Reversible + skippable PCG-style PRNG:
 *   - 64-bit LCG state:   state = state * A + inc   (mod 2^64)
 *   - Output permutation: xorshift + rotate => 32-bit output (PCG-XSH-RR style)
 *   - Reversible stepping: prev() uses modular inverse of A mod 2^64
 *   - Efficient skipping: advance(delta) in O(log |delta|) via affine exponentiation
 *
 * Works well on ESP32-P4 (ESP-IDF). Uses uint64_t; RISC-V handles this fine.
 */

#ifndef PCG32_REVERSIBLE_H
#define PCG32_REVERSIBLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t state;  // 64-bit internal state
    uint64_t inc;    // must be odd (stream selector)
} pcg32_rng_t;

/* PCG default multiplier (odd => invertible mod 2^64) */
#define PCG32_A UINT64_C(6364136223846793005)

/* Precomputed modular inverse of PCG32_A modulo 2^64 */
#define PCG32_A_INV UINT64_C(13877824140714322085)

/* --- small helpers --- */

static inline uint32_t rotr32(uint32_t x, uint32_t r) {
    r &= 31u;
    return (x >> r) | (x << ((32u - r) & 31u));
}

/* Output permutation (PCG-XSH-RR) based on current state (does NOT advance). */
static inline uint32_t pcg32_output(uint64_t state) {
    uint32_t xorshifted = (uint32_t)(((state >> 18u) ^ state) >> 27u);
    uint32_t rot = (uint32_t)(state >> 59u);
    return rotr32(xorshifted, rot);
}

/* One-step forward / backward on state (no output). */
static inline void pcg32_step(pcg32_rng_t *rng) {
    rng->state = rng->state * PCG32_A + rng->inc;
}

static inline void pcg32_unstep(pcg32_rng_t *rng) {
    /* state_{n-1} = A^-1 * (state_n - inc) mod 2^64 */
    rng->state = PCG32_A_INV * (rng->state - rng->inc);
}

/* --- public API --- */

/*
 * Initialize with a seed and a stream id.
 * stream selects an independent sequence; any value is fine.
 */
static inline void pcg32_seed(pcg32_rng_t *rng, uint64_t seed, uint64_t stream) {
    rng->state = 0;
    rng->inc   = (stream << 1u) | 1u;  // must be odd

    /* PCG recommended seeding sequence */
    pcg32_step(rng);
    rng->state += seed;
    pcg32_step(rng);
}

/*
 * Next random 32-bit value.
 * (Output is from current state, then we advance.)
 */
static inline uint32_t pcg32_next_u32(pcg32_rng_t *rng) {
    uint32_t out = pcg32_output(rng->state);
    pcg32_step(rng);
    return out;
}

/*
 * Previous random 32-bit value.
 * (We step backward first, then output from that state.)
 */
static inline uint32_t pcg32_prev_u32(pcg32_rng_t *rng) {
    pcg32_unstep(rng);
    return pcg32_output(rng->state);
}

/*
 * If you sometimes want 64 bits, this is a simple, decent approach:
 * combine two 32-bit outputs.
 */
static inline uint64_t pcg32_next_u64(pcg32_rng_t *rng) {
    uint64_t hi = (uint64_t)pcg32_next_u32(rng);
    uint64_t lo = (uint64_t)pcg32_next_u32(rng);
    return (hi << 32) | lo;
}

/* --- skipping / jumping --- */

/* Affine transform on uint64 mod 2^64: x' = mul*x + add */
typedef struct {
    uint64_t mul;
    uint64_t add;
} affine64_t;

static inline affine64_t affine_compose(affine64_t t2, affine64_t t1) {
    /* t2(t1(x)) */
    affine64_t out;
    out.mul = t2.mul * t1.mul;
    out.add = t2.mul * t1.add + t2.add;
    return out;
}

static inline affine64_t affine_pow(affine64_t base, uint64_t exp) {
    affine64_t result = (affine64_t){ .mul = 1u, .add = 0u }; // identity
    while (exp) {
        if (exp & 1u) result = affine_compose(base, result);
        base = affine_compose(base, base);
        exp >>= 1u;
    }
    return result;
}

/*
 * Advance the generator by a signed delta efficiently.
 * delta > 0: jump forward delta steps
 * delta < 0: jump backward |delta| steps
 *
 * Complexity: O(log |delta|) (<= 64 iterations for 64-bit delta).
 */
static inline void pcg32_advance(pcg32_rng_t *rng, int64_t delta) {
    if (delta == 0) return;

    if (delta > 0) {
        affine64_t step = (affine64_t){ .mul = PCG32_A, .add = rng->inc };
        affine64_t t = affine_pow(step, (uint64_t)delta);
        rng->state = t.mul * rng->state + t.add;
        return;
    }

    /* Backward step: x_{n-1} = A_INV*x_n + (-inc * A_INV) */
    affine64_t back_step = (affine64_t){
        .mul = PCG32_A_INV,
        .add = (uint64_t)(0u - rng->inc) * PCG32_A_INV
    };
    uint64_t k = (uint64_t)(-(delta + 1)) + 1; /* careful abs for INT64_MIN-safe-ish pattern */
    /* The above computes k = (uint64_t)(-delta) without UB in typical compilers.
       If you need strict INT64_MIN handling, clamp or accept that edge case separately. */
    affine64_t t = affine_pow(back_step, k);
    rng->state = t.mul * rng->state + t.add;
}

#ifdef __cplusplus
}
#endif

#endif // PCG32_REVERSIBLE_H

