#include "scaler_nn.h"

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"

static const char *TAG = "scaler_nn";

// Precomputed maps for sizes {16, 32, 64, 128}
static nn_map_t s_maps[SCALER_NN_VALID_SIZES];
static bool s_maps_initialized = false;

// Valid source sizes
static const int s_valid_sizes[SCALER_NN_VALID_SIZES] = SCALER_NN_VALID_SIZES_LIST;

bool nn_init_map(nn_map_t* m, int wsrc, int wdst)
{
    if (!m) {
        return false;
    }

    if (wdst != SCALER_NN_DST_WIDTH) {
        ESP_LOGE(TAG, "Invalid destination width: %d (must be %d)", wdst, SCALER_NN_DST_WIDTH);
        return false;
    }

    // Validate source width
    bool valid_size = false;
    for (int i = 0; i < SCALER_NN_VALID_SIZES; i++) {
        if (s_valid_sizes[i] == wsrc) {
            valid_size = true;
            break;
        }
    }
    if (!valid_size) {
        ESP_LOGE(TAG, "Invalid source width: %d (must be 16, 32, 64, or 128)", wsrc);
        return false;
    }

    m->wsrc = wsrc;
    m->wdst = wdst;

    // Precompute x_map using center-of-pixel rule: floor((x + 0.5) * wsrc / wdst)
    // Fixed-point arithmetic for accuracy
    const uint32_t scale_factor = ((uint32_t)wsrc << 16) / (uint32_t)wdst;
    uint32_t acc = 0;

    for (int x = 0; x < wdst; x++) {
        // Center-of-pixel: (x + 0.5) * wsrc / wdst
        // In fixed-point: acc = (x << 16) + (1 << 15), then multiply by scale_factor
        acc = ((uint32_t)x << 16) + (1 << 15);  // x + 0.5 in fixed-point
        uint32_t src_x_fp = (acc * (uint32_t)wsrc) / (uint32_t)wdst;
        int src_x = (int)(src_x_fp >> 16);

        // Clamp to valid range
        if (src_x < 0) {
            src_x = 0;
        } else if (src_x >= wsrc) {
            src_x = wsrc - 1;
        }

        m->x_map[x] = (uint16_t)src_x;
    }

    return true;
}

void nn_scale_row_rgb888(const uint8_t* src, int wsrc,
                         const nn_map_t* map,
                         uint8_t* dst_row)
{
    if (!src || !map || !dst_row) {
        return;
    }

    if (map->wsrc != wsrc) {
        ESP_LOGE(TAG, "Map source width (%d) doesn't match actual source width (%d)", map->wsrc, wsrc);
        return;
    }

    // Optimized nearest-neighbor scaling with unrolled loop
    // Process 8 pixels at a time for better cache utilization
    const uint8_t* src_px;
    uint8_t* dst_px = dst_row;
    const uint16_t* x_map = map->x_map;

    // Unroll 8 pixels per iteration
    int x = 0;
    for (; x < SCALER_NN_DST_WIDTH - 7; x += 8) {
        // Process 8 pixels
        for (int i = 0; i < 8; i++) {
            uint16_t src_x = x_map[x + i];
            src_px = src + (size_t)src_x * 3;
            
            // Copy RGB888 pixel (3 bytes)
            *dst_px++ = src_px[0];  // R
            *dst_px++ = src_px[1];  // G
            *dst_px++ = src_px[2];  // B
        }
    }

    // Handle remaining pixels
    for (; x < SCALER_NN_DST_WIDTH; x++) {
        uint16_t src_x = x_map[x];
        src_px = src + (size_t)src_x * 3;
        
        *dst_px++ = src_px[0];  // R
        *dst_px++ = src_px[1];  // G
        *dst_px++ = src_px[2];  // B
    }
}

const nn_map_t* nn_get_map(int wsrc)
{
    if (!s_maps_initialized) {
        nn_init_all_maps();
    }

    for (int i = 0; i < SCALER_NN_VALID_SIZES; i++) {
        if (s_maps[i].wsrc == wsrc) {
            return &s_maps[i];
        }
    }

    return NULL;
}

void nn_init_all_maps(void)
{
    if (s_maps_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Initializing nearest-neighbor maps for sizes: 16, 32, 64, 128");

    for (int i = 0; i < SCALER_NN_VALID_SIZES; i++) {
        int wsrc = s_valid_sizes[i];
        if (!nn_init_map(&s_maps[i], wsrc, SCALER_NN_DST_WIDTH)) {
            ESP_LOGE(TAG, "Failed to initialize map for size %d", wsrc);
            memset(&s_maps[i], 0, sizeof(nn_map_t));
        } else {
            ESP_LOGD(TAG, "Initialized map: %d -> %d", wsrc, SCALER_NN_DST_WIDTH);
        }
    }

    s_maps_initialized = true;
    ESP_LOGI(TAG, "All nearest-neighbor maps initialized");
}

