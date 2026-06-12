// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Live viewer: a single 720x720 GDI window driven by wall-clock time.
// Mirrors the device manager: blank-delay -> intro-animation -> hold ->
// (idle).  Press SPACE to replay, N/P to switch animation, R to rotate,
// B to cycle bg presets, S to reroll seed, +/- to nudge duration.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"

/* ------------------------------------------------------------------ state */

typedef struct {
    intro_anim_ctx_t ctx;
    int     anim_idx;
    int     bg_idx;
    int     rotation_idx;          /* index into rotations[] below */
    int     duration_ms;           /* intro-animation window */
    LARGE_INTEGER perf_freq;
    LARGE_INTEGER replay_start;    /* QPC at the start of the current run */
    int     finished;              /* true once we are past blank+intro+hold */
    uint8_t *buffer;
    BITMAPINFO bmi;
} viewer_state_t;

static const uint16_t rotations[] = { 0, 90, 180, 270 };
static const int rotation_count = (int)(sizeof(rotations) / sizeof(rotations[0]));

/* ------------------------------------------------------------------ time */

static double qpc_seconds_since(viewer_state_t *vs, LARGE_INTEGER then)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - then.QuadPart) / (double)vs->perf_freq.QuadPart;
}

static void replay(viewer_state_t *vs)
{
    QueryPerformanceCounter(&vs->replay_start);
    vs->finished = 0;
}

/* ------------------------------------------------------------------ apply */

static void apply_anim_choice(viewer_state_t *vs)
{
    if (vs->anim_idx < 0) vs->anim_idx = 0;
    if (vs->anim_idx >= intro_anim_count) vs->anim_idx = intro_anim_count - 1;
}

static void apply_bg_choice(viewer_state_t *vs)
{
    if (vs->bg_idx < 0) vs->bg_idx = harness_bg_preset_count - 1;
    if (vs->bg_idx >= harness_bg_preset_count) vs->bg_idx = 0;
    const harness_bg_preset_t *p = &harness_bg_presets[vs->bg_idx];
    vs->ctx.bg_r = p->r;
    vs->ctx.bg_g = p->g;
    vs->ctx.bg_b = p->b;
}

static void apply_rotation(viewer_state_t *vs)
{
    if (vs->rotation_idx < 0) vs->rotation_idx = rotation_count - 1;
    if (vs->rotation_idx >= rotation_count) vs->rotation_idx = 0;
    vs->ctx.rotation = rotations[vs->rotation_idx];
    harness_compute_logo_position(&vs->ctx);
}

/* ------------------------------------------------------------------ frame */

static void render_one_frame(viewer_state_t *vs)
{
    double elapsed_ms = qpc_seconds_since(vs, vs->replay_start) * 1000.0;
    int total_ms = HARNESS_BLANK_DELAY_MS + vs->duration_ms + HARNESS_HOLD_MS;

    if (elapsed_ms <= 0.0) elapsed_ms = 0.0;
    if (elapsed_ms >= total_ms) {
        vs->finished = 1;
    }

    if (elapsed_ms < HARNESS_BLANK_DELAY_MS) {
        intro_anim_fill_bg(vs->buffer, &vs->ctx);
        return;
    }
    if (elapsed_ms <= HARNESS_BLANK_DELAY_MS + vs->duration_ms) {
        float t = (float)((elapsed_ms - HARNESS_BLANK_DELAY_MS) / (double)vs->duration_ms);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        intro_anim_registry[vs->anim_idx].render(vs->buffer, &vs->ctx, t);
        return;
    }
    /* hold (or post-hold idle): canonical end state, t = 1. */
    intro_anim_registry[vs->anim_idx].render(vs->buffer, &vs->ctx, 1.0f);
}

/* ------------------------------------------------------------------ paint */

/* GDI's DIBs use BGR with bottom-up rows by default. We tell it top-down
 * via negative biHeight so we can hand it our buffer as-is (top row first). */

static void present(HWND hwnd, viewer_state_t *vs)
{
    HDC hdc = GetDC(hwnd);
    StretchDIBits(hdc,
        0, 0, HARNESS_W, HARNESS_H,
        0, 0, HARNESS_W, HARNESS_H,
        vs->buffer, &vs->bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, hdc);
}

/* ------------------------------------------------------------------ title */

static void update_title(HWND hwnd, viewer_state_t *vs)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "%s  [%d/%d]  —  rot=%u  bg=%s  dur=%dms  seed=%u   "
        "[Space=replay  N/P=anim  R=rotate  B=bg  S=seed  +/-=duration  Esc=quit]",
        intro_anim_registry[vs->anim_idx].name,
        vs->anim_idx + 1, intro_anim_count,
        (unsigned)vs->ctx.rotation,
        harness_bg_presets[vs->bg_idx].name,
        vs->duration_ms,
        (unsigned)vs->ctx.seed);
    SetWindowTextA(hwnd, buf);
}

/* ------------------------------------------------------------------ wndproc */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    viewer_state_t *vs = (viewer_state_t *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_KEYDOWN: {
        if (!vs) break;
        int key = (int)wp;
        int handled = 1;
        switch (key) {
        case VK_ESCAPE:                       PostQuitMessage(0); break;
        case VK_SPACE:                        replay(vs); break;
        case 'N':                             vs->anim_idx = (vs->anim_idx + 1) % intro_anim_count;
                                              apply_anim_choice(vs); replay(vs); break;
        case 'P':                             vs->anim_idx = (vs->anim_idx - 1 + intro_anim_count) % intro_anim_count;
                                              apply_anim_choice(vs); replay(vs); break;
        case 'R':                             vs->rotation_idx = (vs->rotation_idx + 1) % rotation_count;
                                              apply_rotation(vs); replay(vs); break;
        case 'B':                             vs->bg_idx = (vs->bg_idx + 1) % harness_bg_preset_count;
                                              apply_bg_choice(vs); replay(vs); break;
        case 'S':                             vs->ctx.seed = vs->ctx.seed * 1664525u + 1013904223u;
                                              replay(vs); break;
        case VK_OEM_PLUS: case VK_ADD:        vs->duration_ms += 500;
                                              if (vs->duration_ms > 7500) vs->duration_ms = 7500;
                                              replay(vs); break;
        case VK_OEM_MINUS: case VK_SUBTRACT:  vs->duration_ms -= 500;
                                              if (vs->duration_ms < 1000) vs->duration_ms = 1000;
                                              replay(vs); break;
        default: handled = 0; break;
        }
        if (handled) update_title(hwnd, vs);
        return 0;
    }
    case WM_ERASEBKGND: return 1;       /* we paint the whole window every frame */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ run */

int harness_viewer_run(void)
{
    static viewer_state_t vs;
    vs.buffer = (uint8_t *)VirtualAlloc(NULL, HARNESS_BUFSZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!vs.buffer) return 1;

    vs.ctx.width = HARNESS_W;
    vs.ctx.height = HARNESS_H;
    vs.ctx.stride = HARNESS_STRIDE;
    vs.ctx.logo_scale = 3;
    vs.ctx.seed = 1;
    vs.duration_ms = HARNESS_DEFAULT_INTRO_MS;
    vs.bg_idx = 0;
    vs.rotation_idx = 0;
    vs.anim_idx = 0;
    apply_bg_choice(&vs);
    apply_rotation(&vs);
    apply_anim_choice(&vs);

    QueryPerformanceFrequency(&vs.perf_freq);

    /* Top-down DIB, BGR888, 24-bit. Stride = width*3, must be 4-byte aligned;
     * 720*3 = 2160 which is a multiple of 4, so we are fine. */
    ZeroMemory(&vs.bmi, sizeof(vs.bmi));
    vs.bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    vs.bmi.bmiHeader.biWidth       = HARNESS_W;
    vs.bmi.bmiHeader.biHeight      = -HARNESS_H;     /* negative = top-down */
    vs.bmi.bmiHeader.biPlanes      = 1;
    vs.bmi.bmiHeader.biBitCount    = 24;
    vs.bmi.bmiHeader.biCompression = BI_RGB;

    HINSTANCE hInst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "p3a_intro_anim_lab";
    RegisterClassA(&wc);

    /* Adjust window rect so the client area is exactly 720x720. */
    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT rc = {0, 0, HARNESS_W, HARNESS_H};
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowA("p3a_intro_anim_lab", "p3a intro-anim-lab",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)&vs);
    update_title(hwnd, &vs);
    ShowWindow(hwnd, SW_SHOW);

    /* Start the first run on launch. */
    replay(&vs);

    /* Manual loop: pump messages, sleep to the active animation's frame
     * budget, render, present. PeekMessage keeps us responsive. */
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        render_one_frame(&vs);
        present(hwnd, &vs);

        int budget = intro_anim_registry[vs.anim_idx].frame_budget_ms;
        if (budget < 5) budget = 5;
        Sleep((DWORD)budget);
    }
out:
    VirtualFree(vs.buffer, 0, MEM_RELEASE);
    return 0;
}
