// SPDX-License-Identifier: Apache-2.0
// Private to the player: a whole file read into memory.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Reads the file at `path` into a new PSRAM buffer, which the caller frees
// with free(). Errors: P3A_ERR_MISSING (no such file), P3A_ERR_NOT_PLAYABLE
// (not a regular file, or over P3A_MAX_ARTWORK_BYTES), P3A_ERR_CORRUPT (empty),
// ESP_ERR_NO_MEM, ESP_FAIL (the card could not be read).
esp_err_t player_file_read(const char *path, uint8_t **data_out, size_t *size_out);
