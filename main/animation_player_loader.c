#include "animation_player_priv.h"
#include "animation_player.h"
#include "channel_player.h"
#include "swap_future.h"
#include "sdio_bus.h"
#include "ota_manager.h"
#include <unistd.h>
#include <errno.h>
#include "freertos/task.h"

// Some tooling configurations may not resolve component include paths reliably for C files.
// Keep explicit prototypes here to avoid "implicit declaration" diagnostics.
bool sdio_bus_is_locked(void);
const char *sdio_bus_get_holder(void);
bool ota_manager_is_checking(void);

// ============================================================================
// Corrupt file deletion safeguard
// ============================================================================
// 
// SAFEGUARD MEASURE: This mechanism prevents accidental cascade deletion of
// good files. It tracks the last time a file was deleted due to corruption
// and only allows deletion if:
//   1. It's the first deletion since boot, OR
//   2. More than 1 hour has passed since the last deletion
//
// This is a conservative safeguard that may need revision based on real-world
// usage patterns. Future improvements could include:
//   - Per-file deletion tracking (to allow re-deletion after re-download)
//   - More sophisticated corruption detection
//   - User-configurable deletion policies
//
// ============================================================================

// Track last time a corrupt file was deleted (milliseconds since boot)
static uint64_t s_last_corrupt_deletion_ms = 0;
static const uint64_t CORRUPT_DELETION_COOLDOWN_MS = 3600000ULL;  // 1 hour

// ============================================================================
// Auto-retry safeguard
// ============================================================================
// Prevents infinite retry loops by only allowing auto-retry after a successful
// swap. This ensures we don't get stuck retrying the same bad file repeatedly.
// ============================================================================
static bool s_last_swap_was_successful = false;

void animation_loader_mark_swap_successful(void)
{
    s_last_swap_was_successful = true;
}

static void discard_failed_swap_request(esp_err_t error, bool is_live_mode_swap)
{
    bool had_swap_request = false;
    
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        had_swap_request = s_swap_requested;
        s_swap_requested = false;
        s_loader_busy = false;

        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }

        xSemaphoreGive(s_buffer_mutex);
    }

    if (had_swap_request) {
        // Live Mode: recovery is handled by live_mode_recover_from_failed_swap().
        // Here we only ensure the system is left in a clean state.
        if (is_live_mode_swap) {
            ESP_LOGW(TAG, "Live Mode swap failed (error: %s). Triggering recovery logic.",
                     esp_err_to_name(error));
            return;
        }

        // SAFEGUARD: Only auto-retry if the last operation was a successful swap
        // This prevents infinite retry loops when encountering consecutive bad files
        if (s_last_swap_was_successful) {
            ESP_LOGW(TAG, "Swap failed (error: %s). Auto-retrying with next item...",
                     esp_err_to_name(error));
            
            // Reset flag - if this retry also fails, we won't retry again
            s_last_swap_was_successful = false;
            
            // Auto-retry: Request swap to the next item (channel already advanced by caller)
            // This ensures we don't get stuck on corrupted/missing files
            esp_err_t retry_err = animation_player_request_swap_current();
            if (retry_err != ESP_OK) {
                ESP_LOGW(TAG, "Auto-retry swap failed: %s. Will retry on next cycle.",
                         esp_err_to_name(retry_err));
            }
        } else {
            ESP_LOGW(TAG, "Swap failed (error: %s). Auto-retry blocked (previous swap was not successful).",
                     esp_err_to_name(error));
        }
    } else {
        ESP_LOGW(TAG, "Failed to load animation (error: %s). System remains responsive.",
                 esp_err_to_name(error));
    }
}

// Clear an in-flight swap request without treating it as a failure (no auto-retry).
// Used for cases where a swap is intentionally ignored.
static void discard_ignored_swap_request(void)
{
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = false;
        s_loader_busy = false;

        // Back buffer should not be populated for ignored swaps, but keep it safe.
        if (s_back_buffer.decoder || s_back_buffer.file_data) {
            unload_animation_buffer(&s_back_buffer);
        }

        xSemaphoreGive(s_buffer_mutex);
    }
}

// Exposed for prefetch-time corruption handling.
bool animation_loader_try_delete_corrupt_vault_file(const char *filepath, esp_err_t error)
{
    if (!filepath || strstr(filepath, "/vault/") == NULL) {
        return false;
    }

    // SAFEGUARD: Only delete if first time since boot OR more than 1 hour since last deletion
    uint64_t current_time_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool can_delete = false;

    if (s_last_corrupt_deletion_ms == 0) {
        can_delete = true;
    } else {
        uint64_t time_since_last;
        if (current_time_ms >= s_last_corrupt_deletion_ms) {
            time_since_last = current_time_ms - s_last_corrupt_deletion_ms;
        } else {
            time_since_last = CORRUPT_DELETION_COOLDOWN_MS;
        }
        if (time_since_last >= CORRUPT_DELETION_COOLDOWN_MS) {
            can_delete = true;
        }
    }

    if (!can_delete) {
        return false;
    }

    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "DELETING CORRUPT VAULT FILE");
    ESP_LOGE(TAG, "File: %s", filepath);
    ESP_LOGE(TAG, "Error: %s", esp_err_to_name(error));
    ESP_LOGE(TAG, "Reason: File failed to decode/prefetch, marking as corrupt");
    ESP_LOGE(TAG, "Action: Deleting file so it can be re-downloaded");
    ESP_LOGE(TAG, "========================================");

    if (unlink(filepath) == 0) {
        s_last_corrupt_deletion_ms = current_time_ms;
        ESP_LOGI(TAG, "Successfully deleted corrupt file. Will be re-downloaded on next channel refresh.");
        return true;
    }

    ESP_LOGW(TAG, "Failed to delete corrupt file: %s (errno=%d)", filepath, errno);
    return false;
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
            
            // CRITICAL: Wait for any in-progress prefetch to complete before loading.
            // The render task may be using the back buffer's decoder and frame buffers
            // for prefetch (outside the mutex). Starting a new load would call
            // unload_animation_buffer() which frees memory the render task is using,
            // causing heap corruption (use-after-free → crash in tlsf_free).
            //
            // Check BOTH flags:
            // - prefetch_pending: prefetch has been requested (loader set it)
            // - prefetch_in_progress: prefetch is actively executing (render task set it)
            if (s_back_buffer.prefetch_pending || s_back_buffer.prefetch_in_progress) {
                ESP_LOGW(TAG, "Loader task: BLOCKED - prefetch active (pending=%d, in_progress=%d), waiting...",
                         (int)s_back_buffer.prefetch_pending, (int)s_back_buffer.prefetch_in_progress);
                xSemaphoreGive(s_buffer_mutex);
                // Wait a bit and retry - the render task will clear the flags soon
                vTaskDelay(pdMS_TO_TICKS(10));
                // Re-queue ourselves by giving the semaphore back
                if (s_loader_sem) {
                    xSemaphoreGive(s_loader_sem);
                }
                continue;
            }
            
            s_loader_busy = true;
            xSemaphoreGive(s_buffer_mutex);
        } else {
            continue;
        }

        // If swap_future_execute() provided an override, use it for this load.
        animation_load_override_t ov = {0};
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            ov = s_load_override;
            if (s_load_override.valid) {
                // Consume one-shot override
                s_load_override.valid = false;
            }
            xSemaphoreGive(s_buffer_mutex);
        }

        // Apply deferred manual cycle (advance/go_back + exit Live Mode) in the loader task context
        // to avoid overflowing the touch task stack.
        if (!ov.valid) {
            bool do_cycle = false;
            bool cycle_forward = true;
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                do_cycle = s_cycle_pending;
                cycle_forward = s_cycle_forward;
                s_cycle_pending = false;
                xSemaphoreGive(s_buffer_mutex);
            }
            if (do_cycle) {
                // Re-run "swap request ignored" checks here (moved from touch path).
                if (display_renderer_is_ui_mode()) {
                    ESP_LOGW(TAG, "Deferred cycle ignored: UI mode active");
                    discard_failed_swap_request(ESP_ERR_INVALID_STATE, false);
                    continue;
                }
                if (animation_player_is_sd_export_locked()) {
                    ESP_LOGW(TAG, "Deferred cycle ignored: SD card is exported over USB");
                    discard_failed_swap_request(ESP_ERR_INVALID_STATE, false);
                    continue;
                }
                if (animation_player_is_sd_paused()) {
                    ESP_LOGW(TAG, "Deferred cycle ignored: SD access paused for OTA");
                    discard_failed_swap_request(ESP_ERR_INVALID_STATE, false);
                    continue;
                }
                if (sdio_bus_is_locked()) {
                    ESP_LOGW(TAG, "Deferred cycle ignored: SDIO bus locked by %s",
                             sdio_bus_get_holder() ? sdio_bus_get_holder() : "unknown");
                    discard_failed_swap_request(ESP_ERR_INVALID_STATE, false);
                    continue;
                }
                if (ota_manager_is_checking()) {
                    ESP_LOGW(TAG, "Deferred cycle ignored: OTA check in progress");
                    discard_failed_swap_request(ESP_ERR_INVALID_STATE, false);
                    continue;
                }
                if (channel_player_get_post_count() == 0) {
                    ESP_LOGW(TAG, "Deferred cycle ignored: no animations available");
                    discard_failed_swap_request(ESP_ERR_NOT_FOUND, false);
                    continue;
                }

                // Manual swaps break synchronization.
                channel_player_exit_live_mode();
                esp_err_t adv_err = cycle_forward ? channel_player_advance() : channel_player_go_back();
                if (adv_err != ESP_OK) {
                    ESP_LOGW(TAG, "Deferred cycle failed: %s", esp_err_to_name(adv_err));
                    discard_failed_swap_request(adv_err, false);
                    continue;
                }
            }
        }

        const sdcard_post_t *post = NULL;
        const char *filepath = NULL;
        asset_type_t type = ASSET_TYPE_WEBP;
        const char *name_for_log = NULL;
        uint32_t start_frame = 0;
        uint64_t start_time_ms = 0;
        uint32_t live_index = 0;

        if (ov.valid) {
            filepath = ov.filepath;
            type = ov.type;
            name_for_log = ov.filepath;
            start_frame = ov.start_frame;
            start_time_ms = ov.start_time_ms;
            live_index = ov.live_index;
            ESP_LOGI(TAG, "Loader task: swap_future override load: %s (type=%d start_frame=%u start_time_ms=%llu)",
                     filepath, (int)type, (unsigned)start_frame, (unsigned long long)start_time_ms);
        } else {
            // Get current post from channel player
            esp_err_t err = channel_player_get_current_post(&post);
            if (err != ESP_OK || !post) {
                ESP_LOGE(TAG, "Loader task: No current post available");
            discard_failed_swap_request(ESP_ERR_NOT_FOUND, false);
                continue;
            }
            filepath = post->filepath;
            type = post->type;
            name_for_log = post->name;
        }

        // If this is a normal swap request (not swap_future) and the target filepath is
        // exactly the same as what we're already playing, ignore the swap entirely.
        // swap_future is exempt because it can carry start alignment (re-sync) semantics.
        if (!ov.valid && filepath) {
            bool same_as_current = false;
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                const char *current_fp = s_front_buffer.filepath;
                if (current_fp && strcmp(current_fp, filepath) == 0) {
                    same_as_current = true;
                }
                xSemaphoreGive(s_buffer_mutex);
            }

            if (same_as_current) {
                ESP_LOGI(TAG, "Loader task: Ignoring swap request (already playing): %s", filepath);
                discard_ignored_swap_request();
                continue;
            }
        }

        ESP_LOGD(TAG, "Loader task: Loading animation '%s' into back buffer", name_for_log ? name_for_log : "(null)");

        // Check if file exists BEFORE trying to load.
        // This distinguishes "missing" from "corrupt decode" (especially for vault).
        bool file_missing = false;
        if (filepath) {
            struct stat st;
            if (stat(filepath, &st) != 0) {
                file_missing = true;
                ESP_LOGW(TAG, "File missing: %s", filepath);
            }
        } else {
            file_missing = true;
        }

        esp_err_t err = file_missing ? ESP_ERR_NOT_FOUND : load_animation_into_buffer(filepath, type, &s_back_buffer,
                                                                                      start_frame, start_time_ms);
        if (err != ESP_OK) {
            bool is_vault_file = filepath && strstr(filepath, "/vault/") != NULL;
            
            if (is_vault_file && file_missing) {
                // File doesn't exist - advance to next and let background refresh re-download it
                if (!ov.valid || !ov.is_live_mode_swap) {
                    ESP_LOGW(TAG, "Skipping missing vault file, advancing to next: %s", filepath);
                    channel_player_advance();
                }
            } else if (file_missing) {
                // Missing non-vault file (e.g., SD card): advance in non-live playback
                if (!ov.valid || !ov.is_live_mode_swap) {
                    ESP_LOGW(TAG, "Skipping missing file, advancing to next: %s", filepath ? filepath : "(null)");
                    channel_player_advance();
                }
            } else if (is_vault_file) {
                // File exists but failed to decode - it's corrupt
                (void)animation_loader_try_delete_corrupt_vault_file(filepath, err);
                // Advance past corrupt file so we don't get stuck
                if (!ov.valid || !ov.is_live_mode_swap) {
                    channel_player_advance();
                }
            } else {
                // Non-vault decode failure (e.g., SD card corrupt): advance in non-live playback
                if (!ov.valid || !ov.is_live_mode_swap) {
                    ESP_LOGW(TAG, "Decode failed, advancing to next: %s", filepath ? filepath : "(null)");
                    channel_player_advance();
                }
            }
            discard_failed_swap_request(err, ov.valid ? ov.is_live_mode_swap : false);

            // Live Mode recovery: skip forward to next candidate (bounded) without stalling.
            if (ov.valid && ov.is_live_mode_swap) {
                void *nav = channel_player_get_navigator();
                if (nav) {
                    (void)live_mode_recover_from_failed_swap(nav, live_index, err);
                }
            }
            continue;
        }

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            s_back_buffer.prefetch_pending = true;
            s_back_buffer.ready = false;
            s_back_buffer.is_live_mode_swap = ov.valid ? ov.is_live_mode_swap : false;
            s_back_buffer.live_index = ov.valid ? live_index : 0;
            if (swap_was_requested) {
                s_swap_requested = true;
                ESP_LOGD(TAG, "Loader task: Swap was requested, will swap after prefetch");
            }
            s_loader_busy = false;
            xSemaphoreGive(s_buffer_mutex);
        }

        ESP_LOGD(TAG, "Loader task: Successfully loaded animation '%s' (prefetch_pending=true)", name_for_log ? name_for_log : "(null)");
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
            // Check longer extensions first (e.g., .jpeg before .jpg), all comparisons are case-insensitive
            if ((len >= 5 && strcasecmp(name + len - 5, ".webp") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".gif") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) ||
                (len >= 4 && strcasecmp(name + len - 4, ".jpg") == 0)) {
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

    const char *animations_dir = ANIMATIONS_PREFERRED_DIR;
    esp_err_t enum_err = sdcard_channel_refresh(animations_dir);
    
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
    buf->native_bytes_per_pixel = 0;
    buf->native_frame_size = 0;

    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);
    buf->upscale_lookup_x = NULL;
    buf->upscale_lookup_y = NULL;
    buf->upscale_src_w = 0;
    buf->upscale_src_h = 0;
    buf->upscale_dst_w = 0;
    buf->upscale_dst_h = 0;
    buf->upscale_offset_x = 0;
    buf->upscale_offset_y = 0;
    buf->upscale_scaled_w = 0;
    buf->upscale_scaled_h = 0;
    buf->upscale_has_borders = false;
    buf->upscale_rotation_built = DISPLAY_ROTATION_0;

    buf->first_frame_ready = false;
    buf->decoder_at_frame_1 = false;
    buf->prefetch_pending = false;
    buf->prefetch_in_progress = false;
    buf->prefetched_first_frame_delay_ms = 1;
    buf->current_frame_delay_ms = 1;
    buf->static_frame_cached = false;
    buf->static_bg_generation = 0;
    buf->start_time_ms = 0;
    buf->start_frame = 0;
    buf->is_live_mode_swap = false;
    buf->live_index = 0;

    free(buf->filepath);
    buf->filepath = NULL;

    buf->ready = false;
    memset(&buf->decoder_info, 0, sizeof(buf->decoder_info));
    buf->asset_index = 0;
}

// ============================================================================
// Upscale map building (aspect-ratio preserving + rotation-aware)
// ============================================================================

static esp_err_t build_upscale_maps_for_buffer(animation_buffer_t *buf, int canvas_w, int canvas_h, display_rotation_t rotation)
{
    if (!buf || canvas_w <= 0 || canvas_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int target_w = EXAMPLE_LCD_H_RES;
    const int target_h = EXAMPLE_LCD_V_RES;

    // Compute scaled rectangle in PHYSICAL framebuffer coordinates (dst_x/dst_y).
    // For 90/270 we swap source dimensions for aspect-ratio decisions (matches visual rotation),
    // but lookup tables always map to the original source axes:
    // - lookup_x maps to source X in [0, canvas_w)
    // - lookup_y maps to source Y in [0, canvas_h)
    const bool swap_src = (rotation == DISPLAY_ROTATION_90 || rotation == DISPLAY_ROTATION_270);
    const int src_w_eff = swap_src ? canvas_h : canvas_w;
    const int src_h_eff = swap_src ? canvas_w : canvas_h;

    int scaled_w = target_w;
    int scaled_h = target_h;

    // Fit-to-screen, preserve aspect ratio (no cropping).
    if ((int64_t)src_w_eff * (int64_t)target_h >= (int64_t)src_h_eff * (int64_t)target_w) {
        // Source wider (or equal): fit width
        scaled_w = target_w;
        scaled_h = (int)(((int64_t)target_w * (int64_t)src_h_eff) / (int64_t)src_w_eff);
    } else {
        // Source taller: fit height
        scaled_h = target_h;
        scaled_w = (int)(((int64_t)target_h * (int64_t)src_w_eff) / (int64_t)src_h_eff);
    }

    if (scaled_w < 1) scaled_w = 1;
    if (scaled_h < 1) scaled_h = 1;
    if (scaled_w > target_w) scaled_w = target_w;
    if (scaled_h > target_h) scaled_h = target_h;

    const int offset_x = (target_w - scaled_w) / 2;
    const int offset_y = (target_h - scaled_h) / 2;
    const bool has_borders = (offset_x > 0) || (offset_y > 0);

    // Lookup lengths depend on rotation because the blitter indexes lookup_x by dst_x for 0/180
    // but by dst_y for 90/270 (and lookup_y vice-versa).
    //
    // IMPORTANT: We do NOT want to free/allocate lookup tables repeatedly under heavy swap/rotate spam.
    // That creates heap churn and amplifies the impact of any latent corruption. Instead we allocate
    // both tables once at a fixed "max" length and only rewrite the active prefix.
    const int used_lookup_x_len = swap_src ? scaled_h : scaled_w; // maps to source X (canvas_w)
    const int used_lookup_y_len = swap_src ? scaled_w : scaled_h; // maps to source Y (canvas_h)
    const int max_len = (target_w > target_h) ? target_w : target_h;

    if (!buf->upscale_lookup_x) {
        buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc((size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!buf->upscale_lookup_x) {
            ESP_LOGE(TAG, "Failed to allocate upscale lookup X (max_len=%d)", max_len);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!buf->upscale_lookup_y) {
        buf->upscale_lookup_y = (uint16_t *)heap_caps_malloc((size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!buf->upscale_lookup_y) {
            ESP_LOGE(TAG, "Failed to allocate upscale lookup Y (max_len=%d)", max_len);
            heap_caps_free(buf->upscale_lookup_x);
            buf->upscale_lookup_x = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    // Build lookup_x -> source X in [0, canvas_w) for the active prefix
    for (int i = 0; i < used_lookup_x_len; ++i) {
        int src_x = (i * canvas_w) / used_lookup_x_len;
        if (src_x >= canvas_w) src_x = canvas_w - 1;
        if (src_x < 0) src_x = 0;
        buf->upscale_lookup_x[i] = (uint16_t)src_x;
    }
    // Fill remainder defensively with last valid value
    if (used_lookup_x_len > 0) {
        const uint16_t last = buf->upscale_lookup_x[used_lookup_x_len - 1];
        for (int i = used_lookup_x_len; i < max_len; ++i) {
            buf->upscale_lookup_x[i] = last;
        }
    }

    // Build lookup_y -> source Y in [0, canvas_h) for the active prefix
    for (int i = 0; i < used_lookup_y_len; ++i) {
        int src_y = (i * canvas_h) / used_lookup_y_len;
        if (src_y >= canvas_h) src_y = canvas_h - 1;
        if (src_y < 0) src_y = 0;
        buf->upscale_lookup_y[i] = (uint16_t)src_y;
    }
    if (used_lookup_y_len > 0) {
        const uint16_t last = buf->upscale_lookup_y[used_lookup_y_len - 1];
        for (int i = used_lookup_y_len; i < max_len; ++i) {
            buf->upscale_lookup_y[i] = last;
        }
    }

    buf->upscale_src_w = canvas_w;
    buf->upscale_src_h = canvas_h;
    buf->upscale_dst_w = target_w;
    buf->upscale_dst_h = target_h;
    buf->upscale_offset_x = offset_x;
    buf->upscale_offset_y = offset_y;
    buf->upscale_scaled_w = scaled_w;
    buf->upscale_scaled_h = scaled_h;
    buf->upscale_has_borders = has_borders;
    buf->upscale_rotation_built = rotation;

    ESP_LOGD(TAG, "Upscale maps: %dx%d -> %dx%d (offset %d,%d, scaled %dx%d, borders=%d, rot=%d)",
             canvas_w, canvas_h, target_w, target_h, offset_x, offset_y, scaled_w, scaled_h,
             (int)has_borders, (int)rotation);

    return ESP_OK;
}

esp_err_t animation_loader_rebuild_upscale_maps(animation_buffer_t *buf, display_rotation_t rotation)
{
    if (!buf || !buf->decoder) {
        return ESP_ERR_INVALID_STATE;
    }
    const int canvas_w = (int)buf->decoder_info.canvas_width;
    const int canvas_h = (int)buf->decoder_info.canvas_height;
    if (canvas_w <= 0 || canvas_h <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return build_upscale_maps_for_buffer(buf, canvas_w, canvas_h, rotation);
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

    // One-time diagnostic (DEBUG): how this asset flows through the pipeline.
    {
        uint8_t br = 0, bg = 0, bb = 0;
        config_store_get_background_color(&br, &bg, &bb);
        const char *pf = (buf->decoder_info.pixel_format == ANIMATION_PIXEL_FORMAT_RGB888) ? "RGB888" : "RGBA8888";
        ESP_LOGD(TAG, "Decoder: %ux%u frames=%u transp=%d fmt=%s bg=(%u,%u,%u)",
                 (unsigned)buf->decoder_info.canvas_width,
                 (unsigned)buf->decoder_info.canvas_height,
                 (unsigned)buf->decoder_info.frame_count,
                 (int)buf->decoder_info.has_transparency,
                 pf,
                 (unsigned)br, (unsigned)bg, (unsigned)bb);
    }

    const int canvas_w = (int)buf->decoder_info.canvas_width;
    const int canvas_h = (int)buf->decoder_info.canvas_height;
    buf->native_bytes_per_pixel = (buf->decoder_info.pixel_format == ANIMATION_PIXEL_FORMAT_RGB888) ? 3 : 4;
    buf->native_frame_size = (size_t)canvas_w * canvas_h * (size_t)buf->native_bytes_per_pixel;

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

    // Build aspect-ratio preserving lookup maps for the CURRENT rotation.
    // If rotation changes later, the maps must be rebuilt.
    esp_err_t map_err = build_upscale_maps_for_buffer(buf, canvas_w, canvas_h, display_renderer_get_rotation());
    if (map_err != ESP_OK) {
        unload_animation_buffer(buf);
        return map_err;
    }

    return ESP_OK;
}

esp_err_t load_animation_into_buffer(const char *filepath, asset_type_t type, animation_buffer_t *buf,
                                     uint32_t start_frame, uint64_t start_time_ms)
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
    buf->prefetch_in_progress = false;

    // Propagate start alignment parameters (used by prefetch_first_frame()).
    buf->start_frame = start_frame;
    buf->start_time_ms = start_time_ms;

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
