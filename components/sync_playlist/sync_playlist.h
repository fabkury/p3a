#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Synchronized Playlist Engine for ESP32-P4
 * → Perfect sync between devices with only NTP + shared seed
 * → Indefinite rewind/forward without storing history
 * → Two modes: PRECISE (sub-second) and FORGIVING (±10–15 s tolerance)
 */

typedef enum {
    SYNC_MODE_PRECISE = 0,   // Uses exact cumulative duration — perfect timing
    SYNC_MODE_FORGIVING = 1  // One animation change every ~avg_duration — ultra robust
} sync_playlist_mode_t;

typedef struct {
    uint32_t   duration_ms;
    // you can add filename, brightness, etc. here
} animation_t;

typedef struct {
    // Call once at boot
    void (*init)(uint64_t master_seed,
                 uint64_t playlist_start_ms,
                 const animation_t* animations,
                 uint32_t count,
                 sync_playlist_mode_t mode);

    // Call whenever you need the current animation (e.g. every frame or every 500 ms)
    // Returns true if the animation changed since last call
    bool (*update)(uint64_t current_time_ms, uint32_t* out_index, uint32_t* out_elapsed_in_anim_ms);

    // Manual control — works in both modes, no history stored
    void (*next)(void);
    void (*prev)(void);
    void (*jump_steps)(int64_t steps);        // +1000 or -5000 etc.

    // Live mode control
    void (*enable_live)(bool enable);
} sync_playlist_t;

extern const sync_playlist_t SyncPlaylist;

/**
 * @brief Query the currently loaded schedule length
 */
uint32_t sync_playlist_get_count(void);

/**
 * @brief Query a scheduled item's duration (ms)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t sync_playlist_get_duration_ms(uint32_t index, uint32_t *out_duration_ms);