// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// --dump: render an animation deterministically across [0, 1] sampled at
// `frame_budget_ms` and write each frame as a 24-bit BMP. Output is
// reproducible given seed and bg.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BMP_FILEHDR;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;            /* negative = top-down */
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BMP_INFOHDR;
#pragma pack(pop)

static int write_bmp_bgr24(const char *path, const uint8_t *buf,
                           int w, int h, int stride)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f) return -1;
    BMP_FILEHDR fh = {0};
    BMP_INFOHDR ih = {0};
    fh.bfType    = 0x4D42;          /* 'BM' */
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + (uint32_t)stride * (uint32_t)h;
    ih.biSize        = sizeof(ih);
    ih.biWidth       = w;
    ih.biHeight      = -h;          /* top-down */
    ih.biPlanes      = 1;
    ih.biBitCount    = 24;
    ih.biCompression = 0;           /* BI_RGB */
    ih.biSizeImage   = (uint32_t)stride * (uint32_t)h;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(buf, (size_t)stride * (size_t)h, 1, f);
    fclose(f);
    return 0;
}

int harness_dump_run(int argc, char **argv)
{
    /* argv[0] = "--dump"; argv[1] = <dir>; then optional --anim/--duration-ms/--seed. */
    if (argc < 2) {
        fprintf(stderr, "usage: --dump <dir> [--anim <name>] [--duration-ms <ms>] [--seed <u32>]\n");
        return 2;
    }
    const char *dir          = argv[1];
    const char *anim_name    = NULL;
    int         duration_ms  = HARNESS_DEFAULT_INTRO_MS;
    uint32_t    seed         = 1;

    for (int i = 2; i + 1 < argc; i += 2) {
        if      (strcmp(argv[i], "--anim") == 0)        anim_name   = argv[i + 1];
        else if (strcmp(argv[i], "--duration-ms") == 0) duration_ms = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--seed") == 0)        seed        = (uint32_t)strtoul(argv[i + 1], NULL, 10);
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 2; }
    }
    if (duration_ms < 1) duration_ms = 1;

    int anim_idx = 0;
    if (anim_name) {
        anim_idx = -1;
        for (int i = 0; i < intro_anim_count; i++) {
            if (strcmp(intro_anim_registry[i].name, anim_name) == 0) { anim_idx = i; break; }
        }
        if (anim_idx < 0) { fprintf(stderr, "unknown anim: %s\n", anim_name); return 2; }
    }

    CreateDirectoryA(dir, NULL);    /* harmless if it already exists */

    intro_anim_ctx_t ctx = {0};
    ctx.width  = HARNESS_W;
    ctx.height = HARNESS_H;
    ctx.stride = HARNESS_STRIDE;
    ctx.bg_r = 0; ctx.bg_g = 0; ctx.bg_b = 0;
    ctx.rotation = 0;
    ctx.logo_scale = 3;
    ctx.seed = seed;
    harness_compute_logo_position(&ctx);

    uint8_t *buf = (uint8_t *)malloc(HARNESS_BUFSZ);
    if (!buf) { fprintf(stderr, "OOM\n"); return 1; }

    int budget = intro_anim_registry[anim_idx].frame_budget_ms;
    if (budget < 1) budget = 40;
    int frames = (duration_ms + budget - 1) / budget + 1;   /* include t=0 and t=1 */

    for (int i = 0; i < frames; i++) {
        float t = (frames > 1) ? (float)i / (float)(frames - 1) : 1.0f;
        intro_anim_registry[anim_idx].render(buf, &ctx, t);
        char path[1024];
        snprintf(path, sizeof(path), "%s\\frame_%04d.bmp", dir, i);
        if (write_bmp_bgr24(path, buf, HARNESS_W, HARNESS_H, HARNESS_STRIDE) != 0) {
            fprintf(stderr, "failed to write %s\n", path);
            free(buf);
            return 1;
        }
    }
    printf("wrote %d frames to %s\n", frames, dir);
    free(buf);
    return 0;
}
