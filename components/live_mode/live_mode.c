#include "live_mode.h"
#include <sys/time.h>
#include "esp_log.h"

static const char *TAG = "live_mode";

uint64_t live_mode_get_wall_clock_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    uint64_t time_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    
    return time_ms;
}

uint64_t live_mode_get_channel_start_time(void *channel)
{
    // For now, use hardcoded epoch
    // Future: could extract creation date from channel metadata
    (void)channel;
    
    return LIVE_MODE_CHANNEL_EPOCH_UNIX;
}

uint64_t live_mode_get_playlist_start_time(uint32_t created_at)
{
    if (created_at > 0) {
        return (uint64_t)created_at;
    }
    
    // Fallback to channel epoch if no creation date
    return LIVE_MODE_CHANNEL_EPOCH_UNIX;
}
