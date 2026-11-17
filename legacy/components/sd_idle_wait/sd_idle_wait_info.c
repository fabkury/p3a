// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file sd_idle_wait_info.c
 * @brief Build-time identity of the SD busy-wait variant (see sd_idle_wait.h).
 */

#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "sd_idle_wait.h"

// Added to components/sdmmc by the esp-idf #19034 patch (exponential CMD13
// back-off). A weak reference resolves to NULL on an unpatched IDF; on a
// patched one the symbol lives in sdmmc_common.o, which is always linked.
extern void sdmmc_poll_delay_and_backoff(uint32_t *period_us) __attribute__((weak));

bool sd_idle_wait_wrap_enabled(void)
{
#if CONFIG_P3A_SD_IDLE_WAIT_WRAP
    return true;
#else
    return false;
#endif
}

bool sd_idle_wait_idf_patched(void)
{
    return sdmmc_poll_delay_and_backoff != NULL;
}
