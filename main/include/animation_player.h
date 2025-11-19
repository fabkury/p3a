#ifndef ANIMATION_PLAYER_H
#define ANIMATION_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t animation_player_init(esp_lcd_panel_handle_t display_handle,
                                uint8_t **lcd_buffers,
                                uint8_t buffer_count,
                                size_t buffer_bytes,
                                size_t row_stride_bytes);

esp_err_t animation_player_load_asset(const char *filepath);

void animation_player_set_paused(bool paused);
void animation_player_toggle_pause(void);
bool animation_player_is_paused(void);
void animation_player_cycle_animation(bool forward);

esp_err_t animation_player_start(void);
void animation_player_deinit(void);

size_t animation_player_get_current_index(void);
esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index);
esp_err_t animation_player_swap_to_index(size_t index);

esp_err_t animation_player_begin_sd_export(void);
esp_err_t animation_player_end_sd_export(void);
bool animation_player_is_sd_export_locked(void);
esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_PLAYER_H
