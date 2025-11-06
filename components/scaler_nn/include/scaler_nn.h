#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCALER_NN_DST_WIDTH 720
#define SCALER_NN_VALID_SIZES 4
#define SCALER_NN_VALID_SIZES_LIST {16, 32, 64, 128}

typedef struct {
    int wsrc;                    // Source width (16, 32, 64, or 128)
    int wdst;                    // Destination width (always 720)
    uint16_t x_map[720];         // Precomputed X mapping: x_map[x] = source X for destination X
} nn_map_t;

/**
 * @brief Initialize a nearest-neighbor map for scaling
 * 
 * Precomputes the mapping from destination pixels to source pixels.
 * Uses center-of-pixel rule: floor((x + 0.5) * wsrc / wdst)
 * 
 * @param m Map structure to initialize
 * @param wsrc Source width (must be 16, 32, 64, or 128)
 * @param wdst Destination width (must be 720)
 * @return true on success, false on invalid parameters
 */
bool nn_init_map(nn_map_t* m, int wsrc, int wdst);

/**
 * @brief Scale a single row from source to destination using nearest-neighbor
 * 
 * Optimized for RGB888 format. Uses precomputed x_map for branchless scaling.
 * 
 * @param src Source row (RGB888, wsrc * 3 bytes)
 * @param wsrc Source width
 * @param map Precomputed map (must match wsrc)
 * @param dst_row Destination row (RGB888, 720 * 3 bytes)
 */
void nn_scale_row_rgb888(const uint8_t* src, int wsrc,
                         const nn_map_t* map,
                         uint8_t* dst_row);

/**
 * @brief Get precomputed map for a given source size
 * 
 * Returns a pointer to a static map for the given size, or NULL if invalid.
 * Maps are precomputed at startup for sizes {16, 32, 64, 128}.
 * 
 * @param wsrc Source width (16, 32, 64, or 128)
 * @return Pointer to map, or NULL if invalid size
 */
const nn_map_t* nn_get_map(int wsrc);

/**
 * @brief Initialize all precomputed maps
 * 
 * Should be called once at startup to precompute maps for all valid sizes.
 */
void nn_init_all_maps(void);

#ifdef __cplusplus
}
#endif

