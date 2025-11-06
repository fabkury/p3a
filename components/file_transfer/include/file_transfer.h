#pragma once

#include "esp_err.h"

/**
 * @brief Initialize file transfer over UART
 * 
 * Sets up UART communication to receive files via serial port.
 * Commands:
 *   - MKDIR:<path> - Create directory
 *   - FILE_WRITE:<path>:<size> - Receive file data
 * 
 * @return ESP_OK on success
 */
esp_err_t file_transfer_init(void);
