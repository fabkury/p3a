// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file http_api_upload.c
 * @brief HTTP API file upload handler
 * 
 * Contains handler for:
 * - POST /upload (multipart file upload)
 */

#include "http_api_internal.h"
#include "sd_path.h"
#include "esp_timer.h"
#include "animation_player.h"
#include "play_scheduler.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

/**
 * POST /upload
 * Handles multipart/form-data file upload, saves to downloads dir, then moves to animations dir
 * Maximum file size: 5 MB
 * Supported formats: WebP, GIF, JPG, JPEG, PNG
 */
static esp_err_t h_post_upload(httpd_req_t *req) {
    const size_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5 MB
    
    // Get dynamic paths
    char DOWNLOADS_DIR[128];
    char ANIMATIONS_DIR[128];
    if (sd_path_get_downloads(DOWNLOADS_DIR, sizeof(DOWNLOADS_DIR)) != ESP_OK ||
        sd_path_get_animations(ANIMATIONS_DIR, sizeof(ANIMATIONS_DIR)) != ESP_OK) {
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to get SD paths\",\"code\":\"PATH_ERROR\"}");
        return ESP_OK;
    }
    
    // Check Content-Type
    char content_type[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing Content-Type\",\"code\":\"MISSING_CONTENT_TYPE\"}");
        return ESP_OK;
    }
    
    // Check if multipart/form-data
    if (strstr(content_type, "multipart/form-data") == NULL) {
        send_json(req, 415, "{\"ok\":false,\"error\":\"Unsupported Content-Type\",\"code\":\"UNSUPPORTED_MEDIA_TYPE\"}");
        return ESP_OK;
    }

    bool sd_locked = animation_player_is_sd_export_locked();
    if (sd_locked) {
        // Drain request body to keep the HTTP connection consistent, then report busy
        size_t remaining = req->content_len;
        char drain_buf[128];
        while (remaining > 0) {
            size_t chunk = (remaining > sizeof(drain_buf)) ? sizeof(drain_buf) : remaining;
            int ret = httpd_req_recv(req, drain_buf, chunk);
            if (ret <= 0) {
                break;
            }
            remaining -= ret;
        }
        send_json(req, 423, "{\"ok\":false,\"error\":\"SD card shared over USB\",\"code\":\"SD_LOCKED\"}");
        return ESP_OK;
    }

    // Extract boundary from Content-Type
    const char *boundary_str = strstr(content_type, "boundary=");
    if (!boundary_str) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"Missing boundary\",\"code\":\"MISSING_BOUNDARY\"}");
        return ESP_OK;
    }
    boundary_str += 9; // Skip "boundary="
    
    char boundary[128];
    size_t boundary_len = 0;
    while (boundary_str[boundary_len] != '\0' && boundary_str[boundary_len] != ';' && boundary_str[boundary_len] != ' ' && boundary_len < sizeof(boundary) - 1) {
        boundary[boundary_len] = boundary_str[boundary_len];
        boundary_len++;
    }
    boundary[boundary_len] = '\0';
    
    // Check Content-Length
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > MAX_FILE_SIZE) {
        send_json(req, 413, "{\"ok\":false,\"error\":\"File size exceeds 5MB limit\",\"code\":\"FILE_TOO_LARGE\"}");
        return ESP_OK;
    }
    
    struct stat st;
    // Ensure downloads directory exists
    if (stat(DOWNLOADS_DIR, &st) != 0) {
        ESP_LOGI(HTTP_API_TAG, "Creating downloads directory: %s", DOWNLOADS_DIR);
        if (mkdir(DOWNLOADS_DIR, 0755) != 0) {
            ESP_LOGE(HTTP_API_TAG, "Failed to create downloads directory: %s", strerror(errno));
            send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to create downloads directory\",\"code\":\"DIR_CREATE_FAIL\"}");
            return ESP_OK;
        }
    }

    // Ensure animations directory exists
    if (stat(ANIMATIONS_DIR, &st) != 0) {
        ESP_LOGI(HTTP_API_TAG, "Creating animations directory: %s", ANIMATIONS_DIR);
        if (mkdir(ANIMATIONS_DIR, 0755) != 0) {
            ESP_LOGE(HTTP_API_TAG, "Failed to create animations directory: %s", strerror(errno));
            send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to create animations directory\",\"code\":\"DIR_CREATE_FAIL\"}");
            return ESP_OK;
        }
    }
    
    // Temporary file path in downloads directory
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/upload_%llu.tmp", DOWNLOADS_DIR, (unsigned long long)(esp_timer_get_time() / 1000));
    
    FILE *fp = NULL;
    if (!sd_locked) {
        fp = fopen(temp_path, "wb");
        if (!fp) {
            ESP_LOGE(HTTP_API_TAG, "Failed to open temp file for writing: %s", strerror(errno));
            send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to open file\",\"code\":\"FILE_OPEN_FAIL\"}");
            return ESP_OK;
        }
    }

    // Build boundary strings (without leading \r\n for initial boundary detection)
    // boundary is max 128 bytes, so we need 2 + 128 = 130 bytes for "--" + boundary
    char boundary_marker[130];
    snprintf(boundary_marker, sizeof(boundary_marker), "--%s", boundary);
    size_t boundary_marker_len = strlen(boundary_marker);

    // boundary_line needs 4 + 128 = 132 bytes for "\r\n--" + boundary
    char boundary_line[132];
    snprintf(boundary_line, sizeof(boundary_line), "\r\n--%s", boundary);
    size_t boundary_line_len = strlen(boundary_line);

    // boundary_end needs 6 + 128 = 134 bytes for "\r\n--" + boundary + "--"
    char boundary_end[134];
    snprintf(boundary_end, sizeof(boundary_end), "\r\n--%s--", boundary);
    size_t boundary_end_len = strlen(boundary_end);
    
    // Buffer for reading - need extra space for boundary matching across chunks
    const size_t BUF_SIZE = RECV_CHUNK + boundary_line_len + 16; // Extra space for overlap
    char *recv_buf = malloc(BUF_SIZE);
    if (!recv_buf) {
        if (fp) {
            fclose(fp);
            unlink(temp_path);
        }
        send_json(req, 500, "{\"ok\":false,\"error\":\"Out of memory\",\"code\":\"OOM\"}");
        return ESP_OK;
    }
    
    size_t total_received = 0;
    bool found_filename = false;
    char filename[256] = {0};
    
    // State machine for multipart parsing
    enum {
        STATE_FIND_INITIAL_BOUNDARY,
        STATE_READ_HEADERS,
        STATE_STREAM_FILE_DATA,
        STATE_DONE
    } state = STATE_FIND_INITIAL_BOUNDARY;
    
    size_t buf_len = 0;  // Total valid data in recv_buf
    bool boundary_found = false;
    
    while ((total_received < content_len || buf_len > 0) && state != STATE_DONE) {
        // Read more data if buffer has space and we haven't received all content
        if (buf_len < BUF_SIZE - 1 && total_received < content_len) {
            int recv_len = httpd_req_recv(req, recv_buf + buf_len, BUF_SIZE - buf_len - 1);
            if (recv_len <= 0) {
                if (recv_len < 0) {
                    ESP_LOGE(HTTP_API_TAG, "Error receiving data: %d", recv_len);
                    break;
                }
                // recv_len == 0 means connection closed, but continue processing buffer
            } else {
                total_received += recv_len;
                buf_len += recv_len;
            }
        }
        
        if (state == STATE_FIND_INITIAL_BOUNDARY) {
            // Look for initial boundary: "--boundary\r\n" (no leading \r\n)
            // Must be at the start of the buffer
            if (buf_len >= boundary_marker_len + 2) {
                if (memcmp(recv_buf, boundary_marker, boundary_marker_len) == 0) {
                    // Found boundary marker, check for \r\n after it
                    if (recv_buf[boundary_marker_len] == '\r' && 
                        recv_buf[boundary_marker_len + 1] == '\n') {
                        // Skip boundary line: "--boundary\r\n"
                        size_t skip = boundary_marker_len + 2;
                        memmove(recv_buf, recv_buf + skip, buf_len - skip);
                        buf_len -= skip;
                        state = STATE_READ_HEADERS;
                        ESP_LOGD(HTTP_API_TAG, "Found initial boundary");
                    }
                } else {
                    // Not a valid boundary, skip one byte and try again
                    memmove(recv_buf, recv_buf + 1, buf_len - 1);
                    buf_len--;
                }
            } else {
                // Not enough data yet, wait for more
                if (buf_len >= BUF_SIZE - 1) {
                    ESP_LOGE(HTTP_API_TAG, "Boundary not found, buffer full");
                    break;
                }
            }
        } else if (state == STATE_READ_HEADERS) {
            // Look for end of headers: \r\n\r\n
            // Headers are text, so we can use strstr safely here
            char *header_end = NULL;
            for (size_t i = 0; i + 3 < buf_len; i++) {
                if (recv_buf[i] == '\r' && recv_buf[i+1] == '\n' && 
                    recv_buf[i+2] == '\r' && recv_buf[i+3] == '\n') {
                    header_end = recv_buf + i;
                    break;
                }
            }
            
            if (header_end) {
                size_t header_end_pos = header_end - recv_buf;
                
                // Extract filename from headers (headers are text, safe to null-terminate temporarily)
                char save_char = recv_buf[header_end_pos];
                recv_buf[header_end_pos] = '\0';
                
                char *cd = strstr(recv_buf, "Content-Disposition:");
                if (cd) {
                    char *fn_start = strstr(cd, "filename=\"");
                    if (fn_start) {
                        fn_start += 10; // Skip "filename=\""
                        char *fn_end = strchr(fn_start, '"');
                        if (fn_end) {
                            size_t fn_len = fn_end - fn_start;
                            if (fn_len < sizeof(filename)) {
                                memcpy(filename, fn_start, fn_len);
                                filename[fn_len] = '\0';
                                found_filename = true;
                            }
                        }
                    }
                }
                
                recv_buf[header_end_pos] = save_char; // Restore
                
                // Skip headers: header_end points to \r\n\r\n, skip all 4 bytes
                size_t skip = header_end_pos + 4;
                memmove(recv_buf, recv_buf + skip, buf_len - skip);
                buf_len -= skip;
                state = STATE_STREAM_FILE_DATA;
                ESP_LOGD(HTTP_API_TAG, "Headers parsed, starting file data");
            } else {
                // Headers not complete yet
                if (buf_len >= 2048) {
                    ESP_LOGE(HTTP_API_TAG, "Headers too long or malformed");
                    break;
                }
            }
        } else if (state == STATE_STREAM_FILE_DATA) {
            // Stream file data until we find a boundary
            // Boundaries are: \r\n--boundary or \r\n--boundary--
            // We need to check for boundaries that might be split across chunks
            // The overlap buffer ensures boundaries split across recv() calls are detected
            
            size_t write_end = buf_len;
            bool found_boundary_in_buf = false;
            
            // Look for boundary_end first (more specific)
            if (buf_len >= boundary_end_len) {
                for (size_t i = 0; i <= buf_len - boundary_end_len; i++) {
                    if (memcmp(recv_buf + i, boundary_end, boundary_end_len) == 0) {
                        // Found end boundary: \r\n--boundary--
                        // File data ends before the \r\n
                        write_end = i;
                        found_boundary_in_buf = true;
                        boundary_found = true;
                        ESP_LOGD(HTTP_API_TAG, "Found end boundary at position %zu", i);
                        break;
                    }
                }
            }
            
            // If not found, look for regular boundary
            if (!found_boundary_in_buf && buf_len >= boundary_line_len) {
                for (size_t i = 0; i <= buf_len - boundary_line_len; i++) {
                    if (memcmp(recv_buf + i, boundary_line, boundary_line_len) == 0) {
                        // Found boundary: \r\n--boundary
                        // File data ends before the \r\n
                        write_end = i;
                        found_boundary_in_buf = true;
                        boundary_found = true;
                        ESP_LOGD(HTTP_API_TAG, "Found boundary at position %zu", i);
                        break;
                    }
                }
            }
            
            if (found_boundary_in_buf) {
                // Write file data up to (but not including) the boundary
                if (write_end > 0) {
                    size_t written = fwrite(recv_buf, 1, write_end, fp);
                    if (written != write_end) {
                        ESP_LOGE(HTTP_API_TAG, "Failed to write file data");
                        break;
                    }
                }
                state = STATE_DONE;
            } else {
                // No boundary found in current buffer
                // Check if we've received all content - if so, boundary must be here or missing
                if (total_received >= content_len) {
                    // We've read all content but haven't found boundary
                    // This shouldn't happen, but try to write remaining data as file content
                    ESP_LOGW(HTTP_API_TAG, "End of content reached but boundary not found, buf_len=%zu", buf_len);
                    if (buf_len > 0) {
                        // Write remaining data - might be incomplete
                        size_t written = fwrite(recv_buf, 1, buf_len, fp);
                        if (written != buf_len) {
                            ESP_LOGE(HTTP_API_TAG, "Failed to write file data");
                        }
                        buf_len = 0;
                    }
                    // Mark as found to avoid error, but file might be incomplete
                    boundary_found = true;
                    state = STATE_DONE;
                } else {
                    // Write data but keep enough for boundary detection overlap
                    size_t safe_write_len = 0;
                    if (buf_len > boundary_line_len) {
                        safe_write_len = buf_len - boundary_line_len;
                        size_t written = fwrite(recv_buf, 1, safe_write_len, fp);
                        if (written != safe_write_len) {
                            ESP_LOGE(HTTP_API_TAG, "Failed to write file data");
                            break;
                        }
                        // Move remaining data to start of buffer
                        memmove(recv_buf, recv_buf + safe_write_len, buf_len - safe_write_len);
                        buf_len -= safe_write_len;
                    } else if (buf_len == BUF_SIZE - 1 && total_received < content_len) {
                        // Buffer is full but we haven't read all content - this is an error
                        ESP_LOGE(HTTP_API_TAG, "Buffer full but boundary not found, cannot continue");
                        break;
                    }
                    // If buf_len <= boundary_line_len and we haven't read all content, 
                    // loop will continue to read more data
                }
            }
        }
    }
    
    // Ensure data is flushed to storage before closing (power-loss safety)
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(recv_buf);
    
    // Validate that we found the boundary and filename
    if (!boundary_found) {
        unlink(temp_path);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Boundary not found or incomplete upload\",\"code\":\"MALFORMED_DATA\"}");
        return ESP_OK;
    }
    
    if (!found_filename || strlen(filename) == 0) {
        unlink(temp_path);
        send_json(req, 400, "{\"ok\":false,\"error\":\"No filename in upload\",\"code\":\"NO_FILENAME\"}");
        return ESP_OK;
    }
    
    // Validate file extension
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        unlink(temp_path);
        send_json(req, 400, "{\"ok\":false,\"error\":\"File must have an extension\",\"code\":\"INVALID_EXTENSION\"}");
        return ESP_OK;
    }
    ext++; // Skip the dot
    
    bool valid_ext = false;
    if (strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0) {
        valid_ext = true;
    }
    
    if (!valid_ext) {
        unlink(temp_path);
        send_json(req, 400, "{\"ok\":false,\"error\":\"Unsupported file type. Use WebP, GIF, JPG, JPEG, or PNG\",\"code\":\"UNSUPPORTED_TYPE\"}");
        return ESP_OK;
    }
    
    // Final destination path in animations directory
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", ANIMATIONS_DIR, filename);
    
    // Clean up orphan .tmp file if it exists (lazy cleanup)
    char tmp_check[516];
    snprintf(tmp_check, sizeof(tmp_check), "%s.tmp", final_path);
    struct stat tmp_st;
    if (stat(tmp_check, &tmp_st) == 0 && S_ISREG(tmp_st.st_mode)) {
        ESP_LOGD(HTTP_API_TAG, "Removing orphan temp file: %s", tmp_check);
        unlink(tmp_check);
    }
    
    // Check if file already exists, delete it if it does
    if (stat(final_path, &st) == 0) {
        ESP_LOGI(HTTP_API_TAG, "File %s already exists, deleting old version", filename);
        if (unlink(final_path) != 0) {
            ESP_LOGW(HTTP_API_TAG, "Failed to delete existing file %s: %s", final_path, strerror(errno));
            // Continue anyway - try to overwrite with rename
        }
    }
    
    // Move file from temp location to final location
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGE(HTTP_API_TAG, "Failed to move file: %s", strerror(errno));
        unlink(temp_path);
        send_json(req, 500, "{\"ok\":false,\"error\":\"Failed to save file\",\"code\":\"FILE_SAVE_FAIL\"}");
        return ESP_OK;
    }
    
    // Use the original filename (no suffix needed)
    const char *final_filename = filename;

    ESP_LOGI(HTTP_API_TAG, "File uploaded successfully: %s", final_filename);

    // Refresh the play_scheduler SD card cache (builds sdcard.bin with filenames stored in entries)
    play_scheduler_refresh_sdcard_cache();

    // Switch to SD card channel - this triggers playback automatically if entries exist
    // (play_scheduler_execute_command calls play_scheduler_next internally when has_entries=true)
    esp_err_t play_err = play_scheduler_play_named_channel("sdcard");

    if (play_err != ESP_OK) {
        ESP_LOGW(HTTP_API_TAG, "Failed to trigger playback: %s", esp_err_to_name(play_err));
        // File is saved, but couldn't trigger playback - still return success
        char json_resp[512];
        snprintf(json_resp, sizeof(json_resp),
                 "{\"ok\":true,\"data\":{\"filename\":\"%s\",\"warning\":\"File saved but playback not started\"}}",
                 final_filename);
        send_json(req, 200, json_resp);
        return ESP_OK;
    }

    ESP_LOGI(HTTP_API_TAG, "Successfully uploaded and triggered playback for: %s", final_filename);
    char json_resp[512];
    snprintf(json_resp, sizeof(json_resp),
             "{\"ok\":true,\"data\":{\"filename\":\"%s\",\"message\":\"File uploaded and playing\"}}",
             final_filename);
    send_json(req, 200, json_resp);
    return ESP_OK;
}

// ---------- Registration Function ----------

void http_api_register_upload_handler(httpd_handle_t server) {
    httpd_uri_t u = {0};

    u.uri = "/upload";
    u.method = HTTP_POST;
    u.handler = h_post_upload;
    u.user_ctx = NULL;
    register_uri_handler_or_log(server, &u);
}

