// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// --check: contract suite for every registered animation.
// Pass = exit 0; any failure -> non-zero exit + per-animation lines.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "p3a_logo.h"

/* ------------------------------------------------------------------ refs */

static void fill_bg_ref(uint8_t *buf, const intro_anim_ctx_t *ctx)
{
    intro_anim_fill_bg(buf, ctx);
}

static void end_state_ref(uint8_t *buf, const intro_anim_ctx_t *ctx)
{
    fill_bg_ref(buf, ctx);
    p3a_logo_blit_pixelwise_bgr888(
        buf, ctx->width, ctx->height, (int)ctx->stride,
        ctx->logo_x, ctx->logo_y,
        255,
        ctx->bg_b, ctx->bg_g, ctx->bg_r,
        ctx->logo_scale, ctx->rotation);
}

/* ------------------------------------------------------------------ canary */

#define CANARY_BYTES 4096
#define CANARY_VAL   0xA5

static uint8_t *alloc_with_canaries(size_t payload, uint8_t **payload_out)
{
    size_t total = payload + 2 * CANARY_BYTES;
    uint8_t *block = (uint8_t *)malloc(total);
    if (!block) return NULL;
    memset(block, CANARY_VAL, CANARY_BYTES);
    memset(block + CANARY_BYTES + payload, CANARY_VAL, CANARY_BYTES);
    *payload_out = block + CANARY_BYTES;
    return block;
}

static int canaries_intact(const uint8_t *block, size_t payload)
{
    for (size_t i = 0; i < CANARY_BYTES; i++) {
        if (block[i] != CANARY_VAL) return 0;
        if (block[CANARY_BYTES + payload + i] != CANARY_VAL) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ matrix */

typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t bg_matrix[] = {
    { 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff },
    { 0xff, 0x80, 0x00 },
    { 0x80, 0x80, 0x80 },   /* same as logo chroma key */
    { 0x10, 0x30, 0x80 },
};
static const int bg_count = (int)(sizeof(bg_matrix) / sizeof(bg_matrix[0]));
static const uint16_t rotations[] = { 0, 90, 180, 270 };
static const int rot_count = (int)(sizeof(rotations) / sizeof(rotations[0]));
static const uint32_t seeds[] = { 1, 0xC0FFEE, 0xDEADBEEF };
static const int seed_count = (int)(sizeof(seeds) / sizeof(seeds[0]));

/* ------------------------------------------------------------------ run */

typedef struct {
    int  total;
    int  failed;
} result_t;

static int check_one(const intro_anim_t *a, result_t *res, char *fail_buf, size_t fail_buf_sz)
{
    int local_fail = 0;

    uint8_t *block_a, *buf_a;
    uint8_t *block_b, *buf_b;
    block_a = alloc_with_canaries(HARNESS_BUFSZ, &buf_a);
    block_b = alloc_with_canaries(HARNESS_BUFSZ, &buf_b);
    if (!block_a || !block_b) { snprintf(fail_buf, fail_buf_sz, "OOM"); free(block_a); free(block_b); return 1; }

    uint8_t *ref = (uint8_t *)malloc(HARNESS_BUFSZ);
    if (!ref) { snprintf(fail_buf, fail_buf_sz, "OOM"); free(block_a); free(block_b); return 1; }

    for (int bi = 0; bi < bg_count; bi++) {
        for (int ri = 0; ri < rot_count; ri++) {
            for (int si = 0; si < seed_count; si++) {
                intro_anim_ctx_t ctx = {0};
                ctx.width = HARNESS_W;
                ctx.height = HARNESS_H;
                ctx.stride = HARNESS_STRIDE;
                ctx.bg_r = bg_matrix[bi].r;
                ctx.bg_g = bg_matrix[bi].g;
                ctx.bg_b = bg_matrix[bi].b;
                ctx.rotation = rotations[ri];
                ctx.logo_scale = 3;
                ctx.seed = seeds[si];
                harness_compute_logo_position(&ctx);

                /* t = 0 must equal flat bg */
                a->render(buf_a, &ctx, 0.0f);
                fill_bg_ref(ref, &ctx);
                if (memcmp(buf_a, ref, HARNESS_BUFSZ) != 0) {
                    if (!local_fail) snprintf(fail_buf, fail_buf_sz,
                        "t=0 mismatch (bg #%d rot=%u seed=%u)", bi, rotations[ri], seeds[si]);
                    local_fail++;
                }

                /* t = 1 must equal canonical end state */
                a->render(buf_a, &ctx, 1.0f);
                end_state_ref(ref, &ctx);
                if (memcmp(buf_a, ref, HARNESS_BUFSZ) != 0) {
                    if (!local_fail) snprintf(fail_buf, fail_buf_sz,
                        "t=1 mismatch (bg #%d rot=%u seed=%u)", bi, rotations[ri], seeds[si]);
                    local_fail++;
                }

                /* determinism: t = 0.5 reproduces given the same ctx */
                a->render(buf_a, &ctx, 0.5f);
                a->render(buf_b, &ctx, 0.5f);
                if (memcmp(buf_a, buf_b, HARNESS_BUFSZ) != 0) {
                    if (!local_fail) snprintf(fail_buf, fail_buf_sz,
                        "non-deterministic at t=0.5 (bg #%d rot=%u seed=%u)", bi, rotations[ri], seeds[si]);
                    local_fail++;
                }

                /* canaries: nothing scribbled outside the buffer */
                if (!canaries_intact(block_a, HARNESS_BUFSZ) ||
                    !canaries_intact(block_b, HARNESS_BUFSZ)) {
                    if (!local_fail) snprintf(fail_buf, fail_buf_sz,
                        "canary trampled (bg #%d rot=%u seed=%u)", bi, rotations[ri], seeds[si]);
                    local_fail++;
                }
            }
        }
    }

    free(ref);
    free(block_a);
    free(block_b);
    res->total++;
    if (local_fail) res->failed++;
    return local_fail ? 1 : 0;
}

int harness_check_run(void)
{
    result_t res = {0};
    int rc = 0;
    for (int i = 0; i < intro_anim_count; i++) {
        char fail_buf[256] = {0};
        int local = check_one(&intro_anim_registry[i], &res, fail_buf, sizeof(fail_buf));
        if (local) {
            printf("FAIL  %-24s  %s\n", intro_anim_registry[i].name, fail_buf);
            rc = 1;
        } else {
            printf("OK    %-24s\n", intro_anim_registry[i].name);
        }
    }
    printf("\n%d animation(s), %d failed\n", res.total, res.failed);
    return rc;
}
