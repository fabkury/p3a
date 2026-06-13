# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 p3a Contributors
#
# Build the intro-animation host harness with MinGW-w64 gcc. No CMake; one .exe.

$ErrorActionPreference = 'Stop'

# Locate gcc: prefer PATH, fall back to the well-known winget install path
$gcc = (Get-Command gcc.exe -ErrorAction SilentlyContinue).Source
if (-not $gcc) {
    $candidate = 'C:\Users\fab\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
    if (Test-Path $candidate) { $gcc = $candidate }
}
if (-not $gcc) { throw "gcc.exe not found. Install MinGW-w64 (e.g. winget install BrechtSanders.WinLibs.POSIX.UCRT) and try again." }

$repoRoot   = (Resolve-Path "$PSScriptRoot\..\..").Path
$labDir     = $PSScriptRoot
$buildDir   = Join-Path $labDir 'build'
$outExe     = Join-Path $buildDir 'intro-anim-lab.exe'
New-Item -ItemType Directory -Force $buildDir | Out-Null

$introAnims = Join-Path $repoRoot 'components\p3a_core\intro_anims'
$includeP3a = Join-Path $repoRoot 'components\p3a_core\include'

$sources = @(
    (Join-Path $labDir 'main.c'),
    (Join-Path $labDir 'harness_common.c'),
    (Join-Path $labDir 'viewer_win32.c'),
    (Join-Path $labDir 'dump.c'),
    (Join-Path $labDir 'checks.c'),
    (Join-Path $repoRoot 'components\p3a_core\p3a_logo.c'),
    (Join-Path $introAnims 'intro_anim_common.c'),
    (Join-Path $introAnims 'intro_anim_registry.c'),
    (Join-Path $introAnims 'ia_smoothstep_fade.c'),
    (Join-Path $introAnims 'ia_pixel_dissolve.c'),
    (Join-Path $introAnims 'ia_iris_wipe.c'),
    (Join-Path $introAnims 'ia_assemble.c'),
    (Join-Path $introAnims 'ia_scanline_reveal.c'),
    (Join-Path $introAnims 'ia_bounce_drop.c'),
    (Join-Path $introAnims 'ia_wave_settle.c'),
    (Join-Path $introAnims 'ia_checker_tiles.c'),
    (Join-Path $introAnims 'ia_pixel_rain.c'),
    (Join-Path $introAnims 'ia_venetian.c'),
    (Join-Path $introAnims 'ia_glitch_settle.c'),
    (Join-Path $introAnims 'ia_typewriter.c'),
    (Join-Path $introAnims 'ia_spiral_reveal.c'),
    (Join-Path $introAnims 'ia_mosaic_shrink.c'),
    (Join-Path $introAnims 'ia_color_emerge.c'),
    (Join-Path $introAnims 'ia_starburst.c'),
    (Join-Path $introAnims 'ia_plasma_dissolve.c'),
    (Join-Path $introAnims 'ia_voronoi_shatter.c'),
    (Join-Path $introAnims 'ia_hue_cycle_lock.c'),
    (Join-Path $introAnims 'ia_blinds_flip.c'),
    (Join-Path $introAnims 'ia_swirl_in.c'),
    (Join-Path $introAnims 'ia_channel_merge.c')
)

$cflags = @(
    '-std=c11',
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-O2',
    "-I$labDir",
    "-I$introAnims",
    "-I$includeP3a"
)

$ldflags = @(
    '-mwindows',     # subsystem:windows; no console window
    '-lgdi32', '-luser32'
)

Write-Host "gcc: $gcc"
Write-Host "out: $outExe"
& $gcc @cflags @sources -o $outExe @ldflags
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
Write-Host "OK"
