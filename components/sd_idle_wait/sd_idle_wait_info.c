// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// Build-time identity of the SD busy-wait variant (see sd_idle_wait.h).

#include "sd_idle_wait.h"

#include "sdkconfig.h"

bool sd_idle_wait_wrap_enabled(void)
{
#if CONFIG_P3A_SD_IDLE_WAIT_WRAP
    return true;
#else
    return false;
#endif
}

// P3A_IDF_HAS_SDMMC_BACKOFF comes from this component's CMakeLists.txt, which
// looks for the function the esp-idf #19034 patch adds to the IDF's own
// sdmmc_common.c. (1.x asked the linker instead, through a weak reference;
// 2.0 has no weak symbols.)
bool sd_idle_wait_idf_patched(void)
{
    return P3A_IDF_HAS_SDMMC_BACKOFF;
}
