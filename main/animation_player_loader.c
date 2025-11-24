#include "animation_player_priv.h"

static void discard_failed_swap_request(size_t failed_asset_index, esp_err_t error)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        bool had_swap_request = s_swap_requested;
        s_swap_requested = false;
        s_loader_busy = false;

        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }

        size_t next_index = get_next_asset_index(failed_asset_index);
        s_next_asset_index = next_index;

        if (next_index == failed_asset_index && s_sd_file_list.health_flags) {
            bool any_healthy = false;
            for (size_t i = 0; i < s_sd_file_list.count; i++) {
                if (s_sd_file_list.health_flags[i]) {
                    any_healthy = true;
                    break;
                }
            }
            if (!any_healthy) {
                ESP_LOGW(TAG, "No healthy animation files available. System remains responsive but will not auto-swap.");
            }
        }

        xSemaphoreGive(s_buffer_mutex);

        if (had_swap_request) {
            ESP_LOGW(TAG, "Discarded swap request for animation index %zu (error: %s). System remains responsive.",
                     failed_asset_index, esp_err_to_name(error));
        } else {
            ESP_LOGW(TAG, "Failed to load animation index %zu (error: %s). System remains responsive.",
                     failed_asset_index, esp_err_to_name(error));
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

        size_t asset_index_to_load;
        bool swap_was_requested = false;

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            asset_index_to_load = s_next_asset_index;
            swap_was_requested = s_swap_requested;
            s_loader_busy = true;
            xSemaphoreGive(s_buffer_mutex);
        } else {
            continue;
        }

        ESP_LOGD(TAG, "Loader task: Loading animation index %zu into back buffer", asset_index_to_load);

        esp_err_t err = load_animation_into_buffer(asset_index_to_load, &s_back_buffer);
        if (err != ESP_OK) {
            discard_failed_swap_request(asset_index_to_load, err);
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

        ESP_LOGD(TAG, "Loader task: Successfully loaded animation index %zu (prefetch_pending=true)", asset_index_to_load);
    }
}

void free_sd_file_list(void)
{
    if (s_sd_file_list.filenames) {
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            free(s_sd_file_list.filenames[i]);
        }
        free(s_sd_file_list.filenames);
        s_sd_file_list.filenames = NULL;
    }
    if (s_sd_file_list.types) {
        free(s_sd_file_list.types);
        s_sd_file_list.types = NULL;
    }
    if (s_sd_file_list.health_flags) {
        free(s_sd_file_list.health_flags);
        s_sd_file_list.health_flags = NULL;
    }
    s_sd_file_list.count = 0;
    s_sd_file_list.current_index = 0;
    if (s_sd_file_list.animations_dir) {
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.animations_dir = NULL;
    }
}

static asset_type_t get_asset_type(const char *filename)
{
    size_t len = strlen(filename);
    if (len >= 5 && strcasecmp(filename + len - 5, ".webp") == 0) {
        return ASSET_TYPE_WEBP;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".gif") == 0) {
        return ASSET_TYPE_GIF;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".png") == 0) {
        return ASSET_TYPE_PNG;
    }
    if (len >= 4 && strcasecmp(filename + len - 4, ".jpg") == 0) {
        return ASSET_TYPE_JPEG;
    }
    if (len >= 5 && strcasecmp(filename + len - 5, ".jpeg") == 0) {
        return ASSET_TYPE_JPEG;
    }
    return ASSET_TYPE_WEBP;
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

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void shuffle_animation_file_list(void)
{
    if (s_sd_file_list.count <= 1) {
        return;
    }

    for (size_t i = s_sd_file_list.count - 1; i > 0; i--) {
        size_t j = esp_random() % (i + 1);

        char *temp_filename = s_sd_file_list.filenames[i];
        s_sd_file_list.filenames[i] = s_sd_file_list.filenames[j];
        s_sd_file_list.filenames[j] = temp_filename;

        asset_type_t temp_type = s_sd_file_list.types[i];
        s_sd_file_list.types[i] = s_sd_file_list.types[j];
        s_sd_file_list.types[j] = temp_type;

        if (s_sd_file_list.health_flags) {
            bool temp_health = s_sd_file_list.health_flags[i];
            s_sd_file_list.health_flags[i] = s_sd_file_list.health_flags[j];
            s_sd_file_list.health_flags[j] = temp_health;
        }
    }

    ESP_LOGI(TAG, "Randomized animation file list order");
}

esp_err_t enumerate_animation_files(const char *dir_path)
{
    free_sd_file_list();

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_FAIL;
    }

    struct dirent *entry;
    size_t anim_count = 0;
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
                anim_count++;
            }
        }
    }
    rewinddir(dir);

    if (anim_count == 0) {
        ESP_LOGW(TAG, "No animation files found in %s", dir_path);
        closedir(dir);
        return ESP_ERR_NOT_FOUND;
    }

    size_t dir_path_len = strlen(dir_path);
    s_sd_file_list.animations_dir = (char *)malloc(dir_path_len + 1);
    if (!s_sd_file_list.animations_dir) {
        ESP_LOGE(TAG, "Failed to allocate directory path string");
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    strcpy(s_sd_file_list.animations_dir, dir_path);

    s_sd_file_list.filenames = (char **)malloc(anim_count * sizeof(char *));
    if (!s_sd_file_list.filenames) {
        ESP_LOGE(TAG, "Failed to allocate filename array");
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.animations_dir = NULL;
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    s_sd_file_list.types = (asset_type_t *)malloc(anim_count * sizeof(asset_type_t));
    if (!s_sd_file_list.types) {
        ESP_LOGE(TAG, "Failed to allocate type array");
        free(s_sd_file_list.filenames);
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.filenames = NULL;
        s_sd_file_list.animations_dir = NULL;
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    s_sd_file_list.health_flags = (bool *)malloc(anim_count * sizeof(bool));
    if (!s_sd_file_list.health_flags) {
        ESP_LOGE(TAG, "Failed to allocate health flags array");
        free(s_sd_file_list.filenames);
        free(s_sd_file_list.types);
        free(s_sd_file_list.animations_dir);
        s_sd_file_list.filenames = NULL;
        s_sd_file_list.types = NULL;
        s_sd_file_list.animations_dir = NULL;
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < anim_count; i++) {
        s_sd_file_list.health_flags[i] = true;
    }

    size_t idx = 0;
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
            bool is_anim = false;
            if (len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) {
                is_anim = true;
            } else if (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) {
                is_anim = true;
            } else if (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) {
                is_anim = true;
            } else if (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0) {
                is_anim = true;
            } else if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) {
                is_anim = true;
            }

            if (is_anim) {
                size_t name_len = strlen(name);
                s_sd_file_list.filenames[idx] = (char *)malloc(name_len + 1);
                if (!s_sd_file_list.filenames[idx]) {
                    for (size_t i = 0; i < idx; i++) {
                        free(s_sd_file_list.filenames[i]);
                    }
                    free(s_sd_file_list.filenames);
                    free(s_sd_file_list.types);
                    free(s_sd_file_list.health_flags);
                    free(s_sd_file_list.animations_dir);
                    s_sd_file_list.filenames = NULL;
                    s_sd_file_list.types = NULL;
                    s_sd_file_list.health_flags = NULL;
                    s_sd_file_list.animations_dir = NULL;
                    closedir(dir);
                    return ESP_ERR_NO_MEM;
                }
                strcpy(s_sd_file_list.filenames[idx], name);
                s_sd_file_list.types[idx] = get_asset_type(name);
                idx++;
            }
        }
    }
    closedir(dir);

    s_sd_file_list.count = anim_count;

    qsort(s_sd_file_list.filenames, s_sd_file_list.count, sizeof(char *), compare_strings);
    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        s_sd_file_list.types[i] = get_asset_type(s_sd_file_list.filenames[i]);
    }

    ESP_LOGI(TAG, "Found %zu animation files in %s", s_sd_file_list.count, dir_path);

    shuffle_animation_file_list();

    s_sd_file_list.current_index = 0;
#if CONFIG_P3A_PICO8_ENABLE
    esp_err_t pico_err = ensure_pico8_resources();
    if (pico_err != ESP_OK) {
        ESP_LOGW(TAG, "PICO-8 buffer init failed: %s", esp_err_to_name(pico_err));
    }
#endif

    return ESP_OK;
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

    esp_err_t enum_err = enumerate_animation_files(found_dir);
    free(found_dir);
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

    free(buf->prefetched_first_frame);
    buf->prefetched_first_frame = NULL;
    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;
    buf->prefetched_first_frame_delay_ms = 1;
    buf->current_frame_delay_ms = 1;

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
        if (src_x >= canvas_w) {
            src_x = canvas_w - 1;
        }
        buf->upscale_lookup_x[dst_x] = (uint16_t)src_x;
    }

    for (int dst_y = 0; dst_y < target_h; ++dst_y) {
        int src_y = (dst_y * canvas_h) / target_h;
        if (src_y >= canvas_h) {
            src_y = canvas_h - 1;
        }
        buf->upscale_lookup_y[dst_y] = (uint16_t)src_y;
    }

    buf->upscale_src_w = canvas_w;
    buf->upscale_src_h = canvas_h;
    buf->upscale_dst_w = target_w;
    buf->upscale_dst_h = target_h;

    return ESP_OK;
}

esp_err_t load_animation_into_buffer(size_t asset_index, animation_buffer_t *buf)
{
    if (!buf) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sd_file_list.count == 0) {
        ESP_LOGE(TAG, "No animation files available");
        return ESP_ERR_NOT_FOUND;
    }

    if (asset_index >= s_sd_file_list.count) {
        ESP_LOGE(TAG, "Invalid asset index: %zu (max: %zu)", asset_index, s_sd_file_list.count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    unload_animation_buffer(buf);

    const char *filename = s_sd_file_list.filenames[asset_index];
    const char *animations_dir = s_sd_file_list.animations_dir;
    asset_type_t type = s_sd_file_list.types[asset_index];

    if (!animations_dir) {
        ESP_LOGE(TAG, "Animations directory not set");
        return ESP_ERR_INVALID_STATE;
    }

    char filepath[512];
    int ret = snprintf(filepath, sizeof(filepath), "%s/%s", animations_dir, filename);
    if (ret < 0 || ret >= (int)sizeof(filepath)) {
        ESP_LOGE(TAG, "File path too long");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *file_data = NULL;
    size_t file_size = 0;
    esp_err_t err = load_animation_file_from_sd(filepath, &file_data, &file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load file from SD: %s", esp_err_to_name(err));
        if (s_sd_file_list.health_flags && asset_index < s_sd_file_list.count) {
            s_sd_file_list.health_flags[asset_index] = false;
            ESP_LOGW(TAG, "Marked file '%s' (index %zu) as unhealthy due to load failure", filename, asset_index);
        }
        return err;
    }

    buf->file_data = file_data;
    buf->file_size = file_size;
    buf->type = type;
    buf->asset_index = asset_index;

    err = init_animation_decoder_for_buffer(buf, type, file_data, file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize animation decoder '%s': %s", filename, esp_err_to_name(err));
        if (s_sd_file_list.health_flags && asset_index < s_sd_file_list.count) {
            s_sd_file_list.health_flags[asset_index] = false;
            ESP_LOGW(TAG, "Marked file '%s' (index %zu) as unhealthy due to decoder initialization failure", filename, asset_index);
        }
        free(file_data);
        buf->file_data = NULL;
        buf->file_size = 0;
        return err;
    }

    if (s_sd_file_list.health_flags && asset_index < s_sd_file_list.count) {
        s_sd_file_list.health_flags[asset_index] = true;
    }

    buf->prefetched_first_frame = (uint8_t *)malloc(s_frame_buffer_bytes);
    if (!buf->prefetched_first_frame) {
        ESP_LOGE(TAG, "Failed to allocate prefetched frame buffer");
        unload_animation_buffer(buf);
        return ESP_ERR_NO_MEM;
    }
    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;

    ESP_LOGI(TAG, "Loaded animation into buffer: %s (index %zu)", filename, asset_index);

    return ESP_OK;
}

size_t get_next_asset_index(size_t current_index)
{
    if (s_sd_file_list.count == 0) {
        return 0;
    }

    if (!s_sd_file_list.health_flags) {
        return (current_index + 1) % s_sd_file_list.count;
    }

    size_t start_index = (current_index + 1) % s_sd_file_list.count;
    size_t checked = 0;

    while (checked < s_sd_file_list.count) {
        if (s_sd_file_list.health_flags[start_index]) {
            return start_index;
        }
        start_index = (start_index + 1) % s_sd_file_list.count;
        checked++;
    }

    return current_index;
}

size_t get_previous_asset_index(size_t current_index)
{
    if (s_sd_file_list.count == 0) {
        return 0;
    }

    if (!s_sd_file_list.health_flags) {
        return (current_index == 0) ? (s_sd_file_list.count - 1) : (current_index - 1);
    }

    size_t start_index = (current_index == 0) ? (s_sd_file_list.count - 1) : (current_index - 1);
    size_t checked = 0;

    while (checked < s_sd_file_list.count) {
        if (s_sd_file_list.health_flags[start_index]) {
            return start_index;
        }
        start_index = (start_index == 0) ? (s_sd_file_list.count - 1) : (start_index - 1);
        checked++;
    }

    return current_index;
}

esp_err_t animation_player_add_file(const char *filename, const char *animations_dir, size_t insert_after_index, size_t *out_index)
{
    if (animation_player_is_sd_export_locked()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!filename || !animations_dir) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ext = strrchr(filename, '.');
    if (!ext) {
        return ESP_ERR_INVALID_ARG;
    }
    ext++;
    bool valid_ext = false;
    if (strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0) {
        valid_ext = true;
    }
    if (!valid_ext) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sd_file_list.animations_dir && strcmp(s_sd_file_list.animations_dir, animations_dir) != 0) {
        free(s_sd_file_list.animations_dir);
        size_t dir_len = strlen(animations_dir);
        s_sd_file_list.animations_dir = (char *)malloc(dir_len + 1);
        if (!s_sd_file_list.animations_dir) {
            return ESP_ERR_NO_MEM;
        }
        strcpy(s_sd_file_list.animations_dir, animations_dir);
    } else if (!s_sd_file_list.animations_dir) {
        size_t dir_len = strlen(animations_dir);
        s_sd_file_list.animations_dir = (char *)malloc(dir_len + 1);
        if (!s_sd_file_list.animations_dir) {
            return ESP_ERR_NO_MEM;
        }
        strcpy(s_sd_file_list.animations_dir, animations_dir);
    }

    for (size_t i = 0; i < s_sd_file_list.count; i++) {
        if (s_sd_file_list.filenames[i] && strcmp(s_sd_file_list.filenames[i], filename) == 0) {
            ESP_LOGW(TAG, "File %s already in list at index %zu", filename, i);
            if (out_index) {
                *out_index = i;
            }
            return ESP_OK;
        }
    }

    size_t new_count = s_sd_file_list.count + 1;
    char **new_filenames = (char **)realloc(s_sd_file_list.filenames, new_count * sizeof(char *));
    if (!new_filenames) {
        return ESP_ERR_NO_MEM;
    }

    asset_type_t *new_types = (asset_type_t *)realloc(s_sd_file_list.types, new_count * sizeof(asset_type_t));
    if (!new_types) {
        free(new_filenames);
        return ESP_ERR_NO_MEM;
    }

    bool *new_health_flags = NULL;
    if (s_sd_file_list.health_flags) {
        new_health_flags = (bool *)realloc(s_sd_file_list.health_flags, new_count * sizeof(bool));
        if (!new_health_flags) {
            free(new_filenames);
            free(new_types);
            return ESP_ERR_NO_MEM;
        }
    } else {
        new_health_flags = (bool *)malloc(new_count * sizeof(bool));
        if (!new_health_flags) {
            free(new_filenames);
            free(new_types);
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < s_sd_file_list.count; i++) {
            new_health_flags[i] = true;
        }
    }

    size_t filename_len = strlen(filename);
    new_filenames[s_sd_file_list.count] = (char *)malloc(filename_len + 1);
    if (!new_filenames[s_sd_file_list.count]) {
        free(new_filenames);
        free(new_types);
        free(new_health_flags);
        return ESP_ERR_NO_MEM;
    }
    strcpy(new_filenames[s_sd_file_list.count], filename);
    new_types[s_sd_file_list.count] = get_asset_type(filename);
    new_health_flags[s_sd_file_list.count] = true;

    s_sd_file_list.filenames = new_filenames;
    s_sd_file_list.types = new_types;
    s_sd_file_list.health_flags = new_health_flags;
    s_sd_file_list.count = new_count;

    size_t insert_index;
    if (s_sd_file_list.count == 1) {
        insert_index = 0;
    } else {
        if (insert_after_index == SIZE_MAX) {
            insert_index = 0;
            for (size_t i = s_sd_file_list.count - 1; i > 0; i--) {
                char *temp_filename = s_sd_file_list.filenames[i];
                s_sd_file_list.filenames[i] = s_sd_file_list.filenames[i - 1];
                s_sd_file_list.filenames[i - 1] = temp_filename;

                asset_type_t temp_type = s_sd_file_list.types[i];
                s_sd_file_list.types[i] = s_sd_file_list.types[i - 1];
                s_sd_file_list.types[i - 1] = temp_type;

                if (s_sd_file_list.health_flags) {
                    bool temp_health = s_sd_file_list.health_flags[i];
                    s_sd_file_list.health_flags[i] = s_sd_file_list.health_flags[i - 1];
                    s_sd_file_list.health_flags[i - 1] = temp_health;
                }
            }
        } else if (insert_after_index >= s_sd_file_list.count - 1) {
            insert_index = s_sd_file_list.count - 1;
        } else {
            insert_index = insert_after_index + 1;
            for (size_t i = s_sd_file_list.count - 1; i > insert_index; i--) {
                char *temp_filename = s_sd_file_list.filenames[i];
                s_sd_file_list.filenames[i] = s_sd_file_list.filenames[i - 1];
                s_sd_file_list.filenames[i - 1] = temp_filename;

                asset_type_t temp_type = s_sd_file_list.types[i];
                s_sd_file_list.types[i] = s_sd_file_list.types[i - 1];
                s_sd_file_list.types[i - 1] = temp_type;

                if (s_sd_file_list.health_flags) {
                    bool temp_health = s_sd_file_list.health_flags[i];
                    s_sd_file_list.health_flags[i] = s_sd_file_list.health_flags[i - 1];
                    s_sd_file_list.health_flags[i - 1] = temp_health;
                }
            }
        }
    }

    if (out_index) {
        *out_index = insert_index;
    }

    ESP_LOGI(TAG, "Added file %s to animation list at index %zu (inserted after index %zu, total: %zu)",
             filename, insert_index, insert_after_index, s_sd_file_list.count);
    return ESP_OK;
}

