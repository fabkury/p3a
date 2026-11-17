// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

#pragma once

/**
 * @file version.h
 * @brief Firmware version information
 * 
 * Version follows Semantic Versioning 2.0 (https://semver.org/)
 * MAJOR.MINOR.PATCH
 *
 * Version is defined in CMakeLists.txt as PROJECT_VER and passed via compile
 * definitions. The root CMakeLists.txt injects FW_VERSION_* (and
 * P3A_API_VERSION) GLOBALLY via add_compile_definitions() before project(), so
 * every component sees the real values automatically — no per-component
 * declaration is needed. The "0.0.0" fallback below only applies if this
 * header is ever used outside that build (e.g. a standalone tool that does not
 * inherit the global definitions).
 */

// Version components (defined by CMake from PROJECT_VER)
#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR  0
#endif
#ifndef FW_VERSION_MINOR
#define FW_VERSION_MINOR  0
#endif
#ifndef FW_VERSION_PATCH
#define FW_VERSION_PATCH  0
#endif

// Version string (defined by CMake from PROJECT_VER)
#ifndef FW_VERSION_STRING
#define FW_VERSION_STRING "0.0.0"
#endif
#define FW_VERSION        FW_VERSION_STRING

// Device model identifier
#define FW_DEVICE_MODEL   "p3a"

// Packed version for comparison (major<<16 | minor<<8 | patch)
#define FW_VERSION_CODE   ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)
