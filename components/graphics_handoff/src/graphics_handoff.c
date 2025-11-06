#include "graphics_handoff.h"

#include "p3a_hal/display.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_lcd_mipi_dsi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "graphics_handoff";

static SemaphoreHandle_t s_handoff_mutex = NULL;
static bool s_player_mode = false;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
typedef struct {
    SemaphoreHandle_t player_sem;
    SemaphoreHandle_t lvgl_sem;
    lv_display_t *lvgl_display;
    bool use_vsync;
} player_panel_event_ctx_t;

typedef struct {
    uint8_t disp_type;
    void *io_handle;
    esp_lcd_panel_handle_t panel_handle;
    void *control_handle;
    lvgl_port_rotation_cfg_t rotation;
    lv_color_t *draw_buffs[3];
    uint8_t *oled_buffer;
    lv_display_t *disp_drv;
    lv_display_rotation_t current_rotation;
    SemaphoreHandle_t trans_sem;
} lvgl_port_display_ctx_compat_t;

static void* s_trans_sem = NULL;
static lv_display_t* s_lvgl_display = NULL;
static player_panel_event_ctx_t s_panel_event_ctx = {
    .player_sem = NULL,
    .lvgl_sem = NULL,
    .lvgl_display = NULL,
    .use_vsync = false,
};

static IRAM_ATTR bool player_panel_event_cb(esp_lcd_panel_handle_t panel,
                                            esp_lcd_dpi_panel_event_data_t *edata,
                                            void *user_ctx)
{
    player_panel_event_ctx_t *ctx = (player_panel_event_ctx_t *)user_ctx;
    if (!ctx) {
        return false;
    }

    BaseType_t need_yield = pdFALSE;

    if (ctx->player_sem) {
        xSemaphoreGiveFromISR(ctx->player_sem, &need_yield);
    }

    if (ctx->use_vsync) {
        if (ctx->lvgl_sem) {
            xSemaphoreGiveFromISR(ctx->lvgl_sem, &need_yield);
        }
    } else if (ctx->lvgl_display) {
        lv_disp_flush_ready(ctx->lvgl_display);
    }

    return (need_yield == pdTRUE);
}

esp_err_t graphics_handoff_init(void)
{
    ESP_LOGI(TAG, "=== Graphics handoff init start ===");
    
    if (s_handoff_mutex) {
        ESP_LOGW(TAG, "Graphics handoff already initialized");
        return ESP_OK;
    }

    s_handoff_mutex = xSemaphoreCreateMutex();
    if (!s_handoff_mutex) {
        ESP_LOGE(TAG, "Failed to create handoff mutex");
        return ESP_ERR_NO_MEM;
    }

    s_player_mode = false;
    s_panel_handle = NULL;
    if (s_panel_event_ctx.player_sem) {
        vSemaphoreDelete(s_panel_event_ctx.player_sem);
        s_panel_event_ctx.player_sem = NULL;
    }
    s_panel_event_ctx.lvgl_sem = NULL;
    s_panel_event_ctx.lvgl_display = NULL;
    s_panel_event_ctx.use_vsync = false;
    s_trans_sem = NULL;
    s_lvgl_display = NULL;

    ESP_LOGI(TAG, "=== Graphics handoff initialized ===");
    return ESP_OK;
}

esp_err_t graphics_handoff_enter_player_mode(esp_lcd_panel_handle_t* panel_out, void** trans_sem_out)
{
    ESP_LOGI(TAG, "=== Entering player mode ===");
    
    if (!s_handoff_mutex) {
        ESP_LOGE(TAG, "Graphics handoff not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!panel_out) {
        ESP_LOGE(TAG, "panel_out is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_handoff_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take handoff mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (s_player_mode) {
        ESP_LOGW(TAG, "Already in player mode");
        // Already in player mode
        if (panel_out) {
            *panel_out = s_panel_handle;
        }
        if (trans_sem_out) {
            *trans_sem_out = s_trans_sem;
        }
        xSemaphoreGive(s_handoff_mutex);
        return ESP_OK;
    }

    // Get LVGL display handle
    ESP_LOGI(TAG, "Getting LVGL display handle...");
    s_lvgl_display = p3a_hal_display_get_handle();
    if (!s_lvgl_display) {
        ESP_LOGE(TAG, "LVGL display not initialized");
        xSemaphoreGive(s_handoff_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "LVGL display handle: %p", (void*)s_lvgl_display);

    // Lock LVGL display
    ESP_LOGI(TAG, "Locking display...");
    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to lock display");
        xSemaphoreGive(s_handoff_mutex);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Display locked");

    ESP_LOGI(TAG, "Extracting panel handle from LVGL display context...");
    lvgl_port_display_ctx_compat_t *disp_ctx =
        (lvgl_port_display_ctx_compat_t *)lv_display_get_driver_data(s_lvgl_display);
    if (!disp_ctx) {
        ESP_LOGE(TAG, "Display context is NULL");
        bsp_display_unlock();
        xSemaphoreGive(s_handoff_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!disp_ctx->panel_handle) {
        ESP_LOGE(TAG, "Panel handle in display context is NULL");
        bsp_display_unlock();
        xSemaphoreGive(s_handoff_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_panel_handle = disp_ctx->panel_handle;

    // Prepare panel event context
    s_panel_event_ctx.lvgl_display = s_lvgl_display;
    s_panel_event_ctx.lvgl_sem = disp_ctx->trans_sem;
    s_panel_event_ctx.use_vsync = (disp_ctx->trans_sem != NULL);

    if (s_panel_event_ctx.player_sem) {
        vSemaphoreDelete(s_panel_event_ctx.player_sem);
        s_panel_event_ctx.player_sem = NULL;
    }

    s_panel_event_ctx.player_sem = xSemaphoreCreateBinary();
    if (!s_panel_event_ctx.player_sem) {
        ESP_LOGE(TAG, "Failed to create player transfer semaphore");
        bsp_display_unlock();
        xSemaphoreGive(s_handoff_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_trans_sem = (void*)s_panel_event_ctx.player_sem;

    esp_lcd_dpi_panel_event_callbacks_t panel_cbs = {
        .on_color_trans_done = player_panel_event_cb,
        .on_refresh_done = player_panel_event_cb,
    };
    esp_err_t cb_ret = esp_lcd_dpi_panel_register_event_callbacks(s_panel_handle, &panel_cbs, &s_panel_event_ctx);
    if (cb_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register panel callbacks: %s", esp_err_to_name(cb_ret));
        vSemaphoreDelete(s_panel_event_ctx.player_sem);
        s_panel_event_ctx.player_sem = NULL;
        s_trans_sem = NULL;
        bsp_display_unlock();
        xSemaphoreGive(s_handoff_mutex);
        return cb_ret;
    }
    ESP_LOGI(TAG, "Panel handle: %p, player_sem: %p, lvgl_sem: %p",
             (void*)s_panel_handle,
             (void*)s_panel_event_ctx.player_sem,
             (void*)s_panel_event_ctx.lvgl_sem);

    // Stop LVGL port (pauses rendering and timers)
    ESP_LOGI(TAG, "Stopping LVGL port...");
    esp_err_t ret = lvgl_port_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "lvgl_port_stop returned %s (may already be stopped)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LVGL port stopped");
    }

    // Disable LVGL display flush callbacks by removing the flush callback
    // This prevents LVGL from trying to render while we control the panel
    ESP_LOGI(TAG, "Disabling LVGL flush callback...");
    lv_display_set_flush_cb(s_lvgl_display, NULL);

    s_player_mode = true;

    if (panel_out) {
        *panel_out = s_panel_handle;
    }
    if (trans_sem_out) {
        *trans_sem_out = s_trans_sem;
    }

    xSemaphoreGive(s_handoff_mutex);

    ESP_LOGI(TAG, "=== Entered player mode successfully (panel=%p, trans_sem=%p) ===", 
             (void*)s_panel_handle, s_trans_sem);
    return ESP_OK;
}

esp_err_t graphics_handoff_enter_lvgl_mode(void)
{
    if (!s_handoff_mutex) {
        ESP_LOGE(TAG, "Graphics handoff not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_handoff_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_player_mode) {
        // Already in LVGL mode
        xSemaphoreGive(s_handoff_mutex);
        return ESP_OK;
    }

    // Wait for any pending DMA transfers to complete
    if (s_panel_event_ctx.player_sem) {
        SemaphoreHandle_t trans_sem = s_panel_event_ctx.player_sem;
        // Drain any stale semaphore signals
        while (xSemaphoreTake(trans_sem, 0) == pdTRUE) {
            // Drain
        }
        // Wait for current transfer to complete (with timeout)
        if (xSemaphoreTake(trans_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
            xSemaphoreGive(trans_sem);
        } else {
            ESP_LOGW(TAG, "Timeout waiting for DMA transfer completion");
        }
    }

    // Re-enable LVGL display flush callback
    // We need to restore the flush callback that LVGL port uses
    if (s_lvgl_display) {
        // LVGL port will restore the flush callback when resumed
        // For now, we'll let lvgl_port_resume handle it
    }

    // Resume LVGL port (resumes rendering and timers)
    esp_err_t ret = lvgl_port_resume();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "lvgl_port_resume returned %s", esp_err_to_name(ret));
    }

    // Issue LVGL full refresh
    if (s_lvgl_display) {
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(s_lvgl_display);
    }

    s_player_mode = false;
    s_panel_handle = NULL;
    SemaphoreHandle_t player_sem = s_panel_event_ctx.player_sem;
    s_panel_event_ctx.player_sem = NULL;
    s_panel_event_ctx.lvgl_display = s_lvgl_display;
    // Keep LVGL semaphore reference for resume
    if (player_sem) {
        vSemaphoreDelete(player_sem);
    }
    s_trans_sem = NULL;

    // Unlock display (LVGL now owns it)
    bsp_display_unlock();

    xSemaphoreGive(s_handoff_mutex);

    ESP_LOGI(TAG, "Entered LVGL mode");
    return ESP_OK;
}

bool graphics_handoff_is_player_mode(void)
{
    return s_player_mode;
}

