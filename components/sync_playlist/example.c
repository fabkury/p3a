#include "sync_playlist.h"

const animation_t my_playlist[] = {
    { .duration_ms = 12000 },
    { .duration_ms = 18000 },
    { .duration_ms = 25000 },
    // ...
};

void app_main(void) {
    // Put your master seed and start date in NVS or hard-code
    SyncPlaylist.init(
        0xcafef00ddeadbeefULL,          // master_seed (same on every device!)
        1735689600ULL,                  // 2025-01-01 00:00:00 UTC
        my_playlist,
        sizeof(my_playlist)/sizeof(my_playlist[0]),
        SYNC_MODE_FORGIVING             // ← change to PRECISE if you want sub-second sync
    );

    while (1) {
        uint32_t idx, elapsed_ms;
        if (SyncPlaylist.update(time(NULL), &idx, &elapsed_ms)) {
            printf("Now playing animation %u\n", idx);
            start_gif(idx);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}