#include "ui.h"

#include "p3a_hal/display.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui";

typedef struct {
    bool created;
    lv_obj_t *panel;
    lv_obj_t *value_label;
    lv_obj_t *slider;
} ui_context_t;

static ui_context_t s_ui;

static void update_brightness_label(ui_context_t *ctx, int value)
{
    if (ctx->value_label) {
        lv_label_set_text_fmt(ctx->value_label, "%d%%", value);
    }
}

static void brightness_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    ui_context_t *ctx = lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    int32_t value = lv_slider_get_value(slider);
    esp_err_t err = p3a_hal_display_set_brightness((int)value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set brightness: %s", esp_err_to_name(err));
    }
    update_brightness_label(ctx, (int)value);
}

static esp_err_t ensure_ui_created(void)
{
    if (s_ui.created) {
        return ESP_OK;
    }

    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock display for UI creation");
        return ESP_ERR_TIMEOUT;
    }

    lv_display_t *disp = p3a_hal_display_get_handle();
    if (disp == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Display handle not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_t *top = lv_layer_top();

    s_ui.panel = lv_obj_create(top);
    lv_obj_remove_flag(s_ui.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.panel, 280, LV_SIZE_CONTENT);
    lv_obj_align(s_ui.panel, LV_ALIGN_BOTTOM_RIGHT, -24, -24);
    lv_obj_set_style_bg_opa(s_ui.panel, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.panel, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.panel, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_ui.panel, 12, LV_PART_MAIN);
    lv_obj_set_layout(s_ui.panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ui.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.panel,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(s_ui.panel);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(title, "Brightness");

    s_ui.slider = lv_slider_create(s_ui.panel);
    lv_slider_set_range(s_ui.slider, 10, 100);
    s_ui.value_label = lv_label_create(s_ui.panel);
    lv_obj_set_style_text_color(s_ui.value_label, lv_color_white(), LV_PART_MAIN);

    int current_brightness = 90;
    lv_slider_set_value(s_ui.slider, current_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_ui.slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, &s_ui);
    lv_obj_set_width(s_ui.slider, LV_PCT(100));
    update_brightness_label(&s_ui, current_brightness);

    s_ui.created = true;
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI brightness panel created");
    return ESP_OK;
}

void ui_show(void)
{
    if (ensure_ui_created() != ESP_OK) {
        return;
    }

    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock display to show UI");
        return;
    }
    lv_obj_clear_flag(s_ui.panel, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void ui_hide(void)
{
    if (!s_ui.created || !s_ui.panel) {
        return;
    }
    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock display to hide UI");
        return;
    }
    lv_obj_add_flag(s_ui.panel, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

bool ui_is_visible(void)
{
    if (!s_ui.created || !s_ui.panel) {
        return false;
    }
    return !lv_obj_has_flag(s_ui.panel, LV_OBJ_FLAG_HIDDEN);
}
