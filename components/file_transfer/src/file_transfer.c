#include "file_transfer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "storage/fs.h"
#include "ff.h"

static const char *TAG = "file_transfer";

#define UART_NUM UART_NUM_0
#define BUF_SIZE 1024

static TaskHandle_t transfer_task_handle = NULL;
static bool s_initialized = false;

static void file_transfer_task(void *pvParameters)
{
    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
        return;
    }

    char line_buf[512];
    int line_pos = 0;
    bool receiving_file = false;
    char file_path[256] = {0};
    FILE *file_handle = NULL;
    size_t file_size = 0;
    size_t bytes_received = 0;

    ESP_LOGI(TAG, "File transfer task started");

    while (1) {
        // Read from UART hardware buffer directly (bypasses console VFS)
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            if (receiving_file && file_handle) {
                // Receiving file data
                size_t to_write = len;
                if (bytes_received + to_write > file_size) {
                    to_write = file_size - bytes_received;
                }
                
                size_t written = fwrite(data, 1, to_write, file_handle);
                if (written != to_write) {
                    ESP_LOGE(TAG, "Failed to write file data");
                    fclose(file_handle);
                    file_handle = NULL;
                    receiving_file = false;
                    printf("ERROR: Write failed\n");
                    continue;
                }
                
                bytes_received += written;
                
                if (bytes_received >= file_size) {
                    fclose(file_handle);
                    file_handle = NULL;
                    receiving_file = false;
                    ESP_LOGI(TAG, "File received successfully: %s (%zu bytes)", file_path, bytes_received);
                    printf("OK\n");
                    memset(file_path, 0, sizeof(file_path));
                    bytes_received = 0;
                }
            } else {
                // Reading command line
                for (int i = 0; i < len; i++) {
                    char c = (char)data[i];
                    
                    if (c == '\n' || c == '\r') {
                        if (line_pos > 0) {
                            line_buf[line_pos] = '\0';
                            
                            // Process command
                            if (strncmp(line_buf, "MKDIR:", 6) == 0) {
                                char *path = line_buf + 6;
                                ESP_LOGI(TAG, "Creating directory: %s", path);
                                
                                // Create directory using FATFS API
                                // Convert /sdcard/path to 0:/path for FATFS
                                char fatfs_path[256];
                                if (strncmp(path, "/sdcard", 7) == 0) {
                                    int ret = snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", path + 7);
                                    if (ret < 0 || ret >= (int)sizeof(fatfs_path)) {
                                        printf("ERROR: Path too long\n");
                                        continue;
                                    }
                                } else {
                                    int ret = snprintf(fatfs_path, sizeof(fatfs_path), "0:/%s", path);
                                    if (ret < 0 || ret >= (int)sizeof(fatfs_path)) {
                                        printf("ERROR: Path too long\n");
                                        continue;
                                    }
                                }
                                
                                // Create directory recursively
                                FRESULT res = f_mkdir(fatfs_path);
                                if (res == FR_OK || res == FR_EXIST) {
                                    printf("OK\n");
                                    ESP_LOGI(TAG, "Directory created: %s", path);
                                } else {
                                    // Try creating parent directories
                                    char *path_copy = strdup(path);
                                    char *token = strtok(path_copy, "/");
                                    char current_path[256] = "/sdcard";
                                    
                                    while (token) {
                                        strcat(current_path, "/");
                                        strcat(current_path, token);
                                        char fatfs_current[256];
                                        snprintf(fatfs_current, sizeof(fatfs_current), "0:%s", current_path + 7);
                                        f_mkdir(fatfs_current);  // Ignore errors, directory might exist
                                        token = strtok(NULL, "/");
                                    }
                                    
                                    free(path_copy);
                                    printf("OK\n");
                                    ESP_LOGI(TAG, "Directory created: %s", path);
                                }
                            } else if (strncmp(line_buf, "FILE_WRITE:", 11) == 0) {
                                char *rest = line_buf + 11;
                                char *path_end = strchr(rest, ':');
                                if (!path_end) {
                                    printf("ERROR: Invalid format\n");
                                } else {
                                    *path_end = '\0';
                                    strncpy(file_path, rest, sizeof(file_path) - 1);
                                    file_path[sizeof(file_path) - 1] = '\0';
                                    
                                    file_size = (size_t)strtoull(path_end + 1, NULL, 10);
                                    if (file_size == 0 || file_size > 10 * 1024 * 1024) {
                                        printf("ERROR: Invalid size\n");
                                        memset(file_path, 0, sizeof(file_path));
                                    } else {
                                        ESP_LOGI(TAG, "Receiving file: %s (%zu bytes)", file_path, file_size);
                                        
                                        // Ensure parent directory exists
                                        char *last_slash = strrchr(file_path, '/');
                                        if (last_slash && last_slash != file_path) {
                                            *last_slash = '\0';
                                            char fatfs_dir[256];
                                            int ret;
                                            if (strncmp(file_path, "/sdcard", 7) == 0) {
                                                ret = snprintf(fatfs_dir, sizeof(fatfs_dir), "0:%s", file_path + 7);
                                            } else {
                                                ret = snprintf(fatfs_dir, sizeof(fatfs_dir), "0:/%s", file_path);
                                            }
                                            if (ret >= 0 && ret < (int)sizeof(fatfs_dir)) {
                                                f_mkdir(fatfs_dir);  // Ignore errors
                                            }
                                            *last_slash = '/';
                                        }
                                        
                                        // Open file for writing
                                        file_handle = fopen(file_path, "wb");
                                        if (!file_handle) {
                                            printf("ERROR: Cannot open file\n");
                                            memset(file_path, 0, sizeof(file_path));
                                        } else {
                                            receiving_file = true;
                                            bytes_received = 0;
                                            printf("READY\n");
                                        }
                                    }
                                }
                            } else {
                                printf("ERROR: Unknown command\n");
                            }
                            
                            line_pos = 0;
                        }
                    } else if (line_pos < sizeof(line_buf) - 1) {
                        line_buf[line_pos++] = c;
                    }
                }
            }
        } else if (len < 0) {
            // Error reading, wait a bit
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            // No data, wait a bit
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    free(data);
    vTaskDelete(NULL);
}

esp_err_t file_transfer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // UART_NUM_0 is already installed by console VFS, but we can read from hardware buffer directly
    // Get buffered data length to see if UART is accessible
    size_t buffered_len = 0;
    uart_get_buffered_data_len(UART_NUM, &buffered_len);
    ESP_LOGI(TAG, "UART buffered data len: %zu", buffered_len);
    
    // Create file transfer task
    BaseType_t ret = xTaskCreate(
        file_transfer_task,
        "file_transfer",
        4096,
        NULL,
        5,  // Higher priority than console task
        &transfer_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create file transfer task");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "File transfer initialized (reading from UART hardware buffer)");
    return ESP_OK;
}
