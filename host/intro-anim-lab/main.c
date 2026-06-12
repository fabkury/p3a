// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// intro-anim-lab: Windows host harness for p3a intro animations.
//
// Modes:
//   intro-anim-lab.exe                  -> live viewer (default)
//   intro-anim-lab.exe --dump <dir> ... -> deterministic BMP frame dump
//   intro-anim-lab.exe --check          -> automated contract suite
//
// We compile with -mwindows so there is no console; argc/argv come from
// CommandLineToArgvW. Use AttachConsole / AllocConsole when stdout is wanted
// (--check / --dump) so the result is visible from a parent shell.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "harness.h"

static char **argv_utf8_from_wide(int argc, wchar_t **wargv)
{
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) return NULL;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char *)malloc((size_t)n);
        if (!argv[i]) return NULL;
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    return argv;
}

static void attach_console_for_stdio(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return;
    }
    FILE *unused;
    freopen_s(&unused, "CONOUT$", "w", stdout);
    freopen_s(&unused, "CONOUT$", "w", stderr);
    freopen_s(&unused, "CONIN$",  "r", stdin);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nShow;

    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = argv_utf8_from_wide(argc, wargv);

    int rc = 0;
    if (argc <= 1) {
        rc = harness_viewer_run();
    } else if (strcmp(argv[1], "--check") == 0) {
        attach_console_for_stdio();
        rc = harness_check_run();
    } else if (strcmp(argv[1], "--dump") == 0) {
        attach_console_for_stdio();
        rc = harness_dump_run(argc - 1, argv + 1);
    } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        attach_console_for_stdio();
        printf(
            "intro-anim-lab\n"
            "  (no args)              live viewer\n"
            "  --dump <dir> [opts]    deterministic BMP frames -> dir\n"
            "    --anim <name>        animation name (default: registry[0])\n"
            "    --duration-ms <ms>   intro window length (default %d)\n"
            "    --seed <u32>         per-boot seed (default 1)\n"
            "  --check                automated contract suite\n",
            HARNESS_DEFAULT_INTRO_MS);
        rc = 0;
    } else {
        attach_console_for_stdio();
        fprintf(stderr, "unknown argument: %s\n", argv[1]);
        rc = 2;
    }

    LocalFree(wargv);
    return rc;
}
