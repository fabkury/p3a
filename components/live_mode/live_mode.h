#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Channel epoch timestamp (Jan 16, 2026 00:00:00 UTC)
 * 
 * All channels are considered to have started playing at this time
 * for Live Mode synchronization purposes.
 */
#define LIVE_MODE_CHANNEL_EPOCH_UNIX 1737072000ULL

/**
 * @brief Default dwell time for artworks without specified dwell (30 seconds)
 */
#define LIVE_MODE_DEFAULT_DWELL_MS 30000

/**
 * @brief Get current wall-clock time in milliseconds since Unix epoch
 * 
 * Uses system time (from NTP if synchronized, or local RTC if not).
 * Always returns UTC time.
 * 
 * @return Current time in milliseconds since Jan 1, 1970 00:00:00 UTC
 */
uint64_t live_mode_get_wall_clock_ms(void);

/**
 * @brief Get channel start time for Live Mode calculations
 * 
 * Returns the channel epoch timestamp that should be used as the
 * "virtual start time" for infinite loop calculations.
 * 
 * @param channel Channel handle (unused in current implementation)
 * @return Start time in seconds since Unix epoch
 */
uint64_t live_mode_get_channel_start_time(void *channel);

/**
 * @brief Get playlist start time for Live Mode calculations
 * 
 * Uses the post's created_at timestamp as the playlist start time.
 * 
 * @param created_at Post creation timestamp (seconds since Unix epoch)
 * @return Start time in seconds since Unix epoch
 */
uint64_t live_mode_get_playlist_start_time(uint32_t created_at);

#ifdef __cplusplus
}
#endif
