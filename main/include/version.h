#pragma once

/**
 * @file version.h
 * @brief Firmware version information
 * 
 * Version follows Semantic Versioning 2.0 (https://semver.org/)
 * MAJOR.MINOR.PATCH
 */

// Version components
#define FW_VERSION_MAJOR  1
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0

// Version string (must match components above)
#define FW_VERSION        "1.0.0"

// Device model identifier
#define FW_DEVICE_MODEL   "p3a"

// Packed version for comparison (major<<16 | minor<<8 | patch)
#define FW_VERSION_CODE   ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)
