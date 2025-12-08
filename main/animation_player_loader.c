#include "animation_player_priv.h"
#include "channel_player.h"

static void discard_failed_swap_request(esp_err_t error)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        bool had_swap_request = s_swap_requested;
        s_swap_requested = false;
        s_loader_busy = false;

        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }

        xSemaphoreGive(s_buffer_mutex);

        if (had_swap_request) {
            ESP_LOGW(TAG, "Discarded swap request (error: %s). System remains responsive.",
                     esp_err_to_name(error));
        } else {
            ESP_LOGW(TAG, "Failed to load animation (error: %s). System remains responsive.",
                     esp_err_to_name(error));
        }
    }
}

void animation_loader_wait_for_idle(void)
{
    while (true) {
        bool busy = false;
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            busy = s_loader_busy;
            xSemaphoreGive(s_buffer_mutex);
        }
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void animation_loader_task(void *arg)
{
    (void)arg;

    while (true) {
        if (xSemaphoreTake(s_loader_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool swap_was_requested = false;

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            swap_was_requested = s_swap_requested;
            
            // Skip loading if in UI mode and not triggered by exit_ui_mode
            if (display_renderer_is_ui_mode() && !swap_was_requested) {
                ESP_LOGD(TAG, "Loader task: Skipping load during UI mode");
                xSemaphoreGive(s_buffer_mutex);
                continue;
            }
            
            s_loader_busy = true;
            xSemaphoreGive(s_buffer_mutex);
        } else {
            continue;
        }

        // Get current post from channel player
        const sdcard_post_t *post = NULL;
        esp_err_t err = channel_player_get_current_post(&post);
        if (err != ESP_OK || !post) {
            ESP_LOGE(TAG, "Loader task: No current post available");
            discard_failed_swap_request(ESP_ERR_NOT_FOUND);
            continue;
        }

        ESP_LOGD(TAG, "Loader task: Loading animation '%s' into back buffer", post->name);

        err = load_animation_into_buffer(post->filepath, post->type, &s_back_buffer);
        if (err != ESP_OK) {
            discard_failed_swap_request(err);
            continue;
        }

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            s_back_buffer.prefetch_pending = true;
            s_back_buffer.ready = false;
            if (swap_was_requested) {
                s_swap_requested = true;
                ESP_LOGD(TAG, "Loader task: Swap was requested, will swap after prefetch");
            }
            s_loader_busy = false;
            xSemaphoreGive(s_buffer_mutex);
        }

        ESP_LOGD(TAG, "Loader task: Successfully loaded animation '%s' (prefetch_pending=true)", post->name);
    }
}

void free_sd_file_list(void)
{
    // Legacy function - no longer needed with channel abstraction
    (void)s_sd_file_list;
}

bool directory_has_animation_files(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "directory_has_animation_files: Failed to open %s", dir_path);
        return false;
    }

    struct dirent *entry;
    bool has_anim = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            const char *name = entry->d_name;
            size_t len = strlen(name);
            if ((len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0)) {
                has_anim = true;
                break;
            }
        }
    }
    closedir(dir);

    return has_anim;
}

esp_err_t find_animations_directory(const char *root_path, char **found_dir_out)
{
    ESP_LOGI(TAG, "Searching in: %s", root_path);

    DIR *dir = opendir(root_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno: %d)", root_path, errno);
        return ESP_FAIL;
    }

    if (directory_has_animation_files(root_path)) {
        size_t len = strlen(root_path);
        *found_dir_out = (char *)malloc(len + 1);
        if (!*found_dir_out) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        strcpy(*found_dir_out, root_path);
        closedir(dir);
        ESP_LOGI(TAG, "Found animations directory: %s", *found_dir_out);
        return ESP_OK;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char subdir_path[512];
        int ret = snprintf(subdir_path, sizeof(subdir_path), "%s/%s", root_path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(subdir_path)) {
            continue;
        }

        struct stat st;
        if (stat(subdir_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = find_animations_directory(subdir_path, found_dir_out);
            if (err == ESP_OK) {
                closedir(dir);
                return ESP_OK;
            }
        }
    }

    closedir(dir);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t enumerate_animation_files(const char *dir_path)
{
    esp_err_t err = sdcard_channel_refresh(dir_path);
    if (err == ESP_OK) {
        channel_player_load_channel();
    }
    return err;
}

static esp_err_t load_animation_file_from_sd(const char *filepath, uint8_t **data_out, size_t *size_out)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = (uint8_t *)malloc((size_t)file_size);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %ld bytes for animation file", file_size);
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, f);
    fclose(f);

    if (bytes_read != (size_t)file_size) {
        ESP_LOGE(TAG, "Failed to read complete file: read %zu of %ld bytes", bytes_read, file_size);
        free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    *data_out = buffer;
    *size_out = (size_t)file_size;

    return ESP_OK;
}

esp_err_t refresh_animation_file_list(void)
{
    if (!s_sd_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char *found_dir = NULL;
    esp_err_t err = find_animations_directory(BSP_SD_MOUNT_POINT, &found_dir);
    if (err != ESP_OK || !found_dir) {
        if (found_dir) {
            free(found_dir);
        }
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    esp_err_t enum_err = sdcard_channel_refresh(found_dir);
    free(found_dir);
    
    if (enum_err == ESP_OK) {
        channel_player_load_channel();
    }
    
    return enum_err;
}

void unload_animation_buffer(animation_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    animation_decoder_unload(&buf->decoder);

    if (buf->file_data) {
        free((void *)buf->file_data);
        buf->file_data = NULL;
        buf->file_size = 0;
    }

    free(buf->native_frame_b1);
    free(buf->native_frame_b2);
    buf->native_frame_b1 = NULL;
    buf->native_frame_b2 = NULL;
    buf->native_buffer_active = 0;
    buf->native_frame_size = 0;

    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);
    buf->upscale_lookup_x = NULL;
    buf->upscale_lookup_y = NULL;
    buf->upscale_src_w = 0;
    buf->upscale_src_h = 0;
    buf->upscale_dst_w = 0;
    buf->upscale_dst_h = 0;

    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;
    buf->prefetched_first_frame_delay_ms = 1;
    buf->current_frame_delay_ms = 1;

    free(buf->filepath);
    buf->filepath = NULL;

    buf->ready = false;
    memset(&buf->decoder_info, 0, sizeof(buf->decoder_info));
    buf->asset_index = 0;
}

static esp_err_t init_animation_decoder_for_buffer(animation_buffer_t *buf, asset_type_t type, const uint8_t *data, size_t size)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

    animation_decoder_type_t decoder_type;
    if (type == ASSET_TYPE_WEBP) {
        decoder_type = ANIMATION_DECODER_TYPE_WEBP;
    } else if (type == ASSET_TYPE_GIF) {
        decoder_type = ANIMATION_DECODER_TYPE_GIF;
    } else if (type == ASSET_TYPE_PNG) {
        decoder_type = ANIMATION_DECODER_TYPE_PNG;
    } else if (type == ASSET_TYPE_JPEG) {
        decoder_type = ANIMATION_DECODER_TYPE_JPEG;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = animation_decoder_init(&buf->decoder, decoder_type, data, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize decoder");
        return err;
    }

    err = animation_decoder_get_info(buf->decoder, &buf->decoder_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get decoder info");
        animation_decoder_unload(&buf->decoder);
        return err;
    }

    const int canvas_w = (int)buf->decoder_info.canvas_width;
    const int canvas_h = (int)buf->decoder_info.canvas_height;
    buf->native_frame_size = (size_t)canvas_w * canvas_h * 4;

    buf->native_frame_b1 = (uint8_t *)malloc(buf->native_frame_size);
    if (!buf->native_frame_b1) {
        ESP_LOGE(TAG, "Failed to allocate native frame buffer B1");
        animation_decoder_unload(&buf->decoder);
        return ESP_ERR_NO_MEM;
    }

    buf->native_frame_b2 = (uint8_t *)malloc(buf->native_frame_size);
    if (!buf->native_frame_b2) {
        ESP_LOGE(TAG, "Failed to allocate native frame buffer B2");
        free(buf->native_frame_b1);
        buf->native_frame_b1 = NULL;
        animation_decoder_unload(&buf->decoder);
        return ESP_ERR_NO_MEM;
    }

    buf->native_buffer_active = 0;

    const int target_w = EXAMPLE_LCD_H_RES;
    const int target_h = EXAMPLE_LCD_V_RES;

    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);

    buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc((size_t)target_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf->upscale_lookup_x) {
        ESP_LOGE(TAG, "Failed to allocate upscale lookup X");
        unload_animation_buffer(buf);
        return ESP_ERR_NO_MEM;
    }

    buf->upscale_lookup_y = (uint16_t *)heap_caps_malloc((size_t)target_h * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf->upscale_lookup_y) {
        ESP_LOGE(TAG, "Failed to allocate upscale lookup Y");
        unload_animation_buffer(buf);
        return ESP_ERR_NO_MEM;
    }

            for (int dst_x = 0; dst_x < target_w; ++dst_x) {
                int src_x = (dst_x * canvas_w) / target_w;
                if (src_x >= canvas_w) src_x = canvas_w - 1;
                buf->upscale_lookup_x[dst_x] = (uint16_t)src_x;
            }
            for (int dst_y = 0; dst_y < target_h; ++dst_y) {
                int src_y = (dst_y * canvas_h) / target_h;
                if (src_y >= canvas_h) src_y = canvas_h - 1;
                buf->upscale_lookup_y[dst_y] = (uint16_t)src_y;
    }

    buf->upscale_src_w = canvas_w;
    buf->upscale_src_h = canvas_h;
    buf->upscale_dst_w = target_w;
    buf->upscale_dst_h = target_h;

    return ESP_OK;
}

esp_err_t load_animation_into_buffer(const char *filepath, asset_type_t type, animation_buffer_t *buf)
{
    if (!buf || !filepath) {
        return ESP_ERR_INVALID_ARG;
    }

    unload_animation_buffer(buf);

    uint8_t *file_data = NULL;
    size_t file_size = 0;
    esp_err_t err = load_animation_file_from_sd(filepath, &file_data, &file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load file from SD: %s", esp_err_to_name(err));
        return err;
    }

    buf->file_data = file_data;
    buf->file_size = file_size;
    buf->type = type;
    buf->asset_index = channel_player_get_current_position();

    // Store filepath
    buf->filepath = strdup(filepath);
    if (!buf->filepath) {
        ESP_LOGE(TAG, "Failed to duplicate filepath");
        free(file_data);
        buf->file_data = NULL;
        buf->file_size = 0;
        return ESP_ERR_NO_MEM;
    }

    err = init_animation_decoder_for_buffer(buf, type, file_data, file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize animation decoder '%s': %s", filepath, esp_err_to_name(err));
        free(file_data);
        buf->file_data = NULL;
        buf->file_size = 0;
        free(buf->filepath);
        buf->filepath = NULL;
        return err;
    }

    // No separate prefetch buffer needed - the first frame is decoded to native_frame_b1
    // during prefetch, then upscaled directly to the display back buffer when displayed
    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;

    ESP_LOGI(TAG, "Loaded animation into buffer: %s", filepath);

    return ESP_OK;
}

size_t get_next_asset_index(size_t current_index)
{
    (void)current_index;
    return 0;
}

size_t get_previous_asset_index(size_t current_index)
{
    (void)current_index;
    return 0;
}

esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index)
{
    (void)filename;
    (void)animations_dir;
    (void)insert_after_index;
    (void)out_index;
    
    ESP_LOGW(TAG, "animation_player_add_file: Not supported with channel abstraction.");
    return ESP_ERR_NOT_SUPPORTED;
}
