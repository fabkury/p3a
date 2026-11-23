// SPDX-License-Identifier: Apache-2.0
// Touch: the task that reads the panel's touch controller every 20 ms,
// turns fingers into gestures (ui_gesture.c), asks what a gesture does on
// the screen that is up (ui_route.c), and does it.
//
// A test can stand in for the fingers: p3a_ui_touch_script() queues samples
// that the task then reads instead of the controller, at the same pace, so
// the whole path from sample to action is the one real fingers take.

#include <string.h>

#include "display_renderer.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "p3a_board.h"
#include "p3a_settings.h"
#include "p3a_task.h"
#include "p3a_ui.h"
#include "play_scheduler.h"
#include "ui_gesture.h"
#include "ui_internal.h"
#include "ui_route.h"

static const char *TAG = "ui_touch";

#define POLL_MS      20
#define SCRIPT_STEPS 24

static SemaphoreHandle_t s_lock;  // guards the script and the status
static p3a_ui_touch_step_t s_script[SCRIPT_STEPS];
static size_t s_script_len, s_script_at;
static int64_t s_step_ends_ms;
static p3a_ui_touch_status_t s_status;
static p3a_ui_info_fn s_info;

void p3a_ui_set_info_source(p3a_ui_info_fn fill)
{
    s_info = fill;
}

esp_err_t p3a_ui_touch_script(const p3a_ui_touch_step_t *steps, size_t count)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (steps == NULL || count == 0 || count > SCRIPT_STEPS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;  // one script at a time
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_script_at >= s_script_len) {
        memcpy(s_script, steps, count * sizeof(steps[0]));
        s_script_len = count;
        s_script_at = 0;
        s_step_ends_ms = 0;
        s_status.scripted = true;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void p3a_ui_touch_get_status(p3a_ui_touch_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        *out = s_status;
        xSemaphoreGive(s_lock);
    }
}

// The next sample: from the script while one runs, else from the controller.
static void sample(ui_touch_t *touch, int64_t now_ms)
{
    memset(touch, 0, sizeof(*touch));
    bool scripted = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_script_at < s_script_len) {
        const p3a_ui_touch_step_t *step = &s_script[s_script_at];
        if (s_step_ends_ms == 0) {
            s_step_ends_ms = now_ms + step->hold_ms;
        }
        touch->count = step->fingers > 2 ? 2 : step->fingers;
        touch->x[0] = step->x[0];
        touch->y[0] = step->y[0];
        touch->x[1] = step->x[1];
        touch->y[1] = step->y[1];
        scripted = true;
        if (now_ms >= s_step_ends_ms) {
            s_script_at++;
            s_step_ends_ms = 0;
        }
    } else {
        s_status.scripted = false;
    }
    xSemaphoreGive(s_lock);
    if (scripted) {
        return;
    }
    esp_lcd_touch_handle_t controller = p3a_board_get_touch();
    if (controller == NULL) {
        return;  // no touch this boot: nothing is ever down
    }
    esp_lcd_touch_point_data_t points[2];
    uint8_t count = 0;
    esp_lcd_touch_read_data(controller);
    if (esp_lcd_touch_get_data(controller, points, &count, 2) == ESP_OK) {
        touch->count = count > 2 ? 2 : count;
        for (uint8_t i = 0; i < touch->count; i++) {
            touch->x[i] = points[i].x;
            touch->y[i] = points[i].y;
        }
    }
}

static void turn(int by)
{
    static p3a_settings_t settings;  // too large for this task's stack; only this task uses it
    p3a_settings_get(&settings);
    const int32_t rotation = (((int32_t)settings.rotation + by) % 360 + 360) % 360;
    p3a_settings_set_int(P3A_SET_ROTATION, rotation);
}

static void act(ui_action_t action, p3a_screen_t screen)
{
    p3a_ui_content_t words;
    switch (action) {
    case UI_ACTION_NEXT:
        play_scheduler_next();
        break;
    case UI_ACTION_BACK:
        play_scheduler_back();
        break;
    case UI_ACTION_TURN_CW:
        turn(90);
        break;
    case UI_ACTION_TURN_CCW:
        turn(-90);
        break;
    case UI_ACTION_OPEN_INFO:
        p3a_ui_content(&words, "p3a", -1, "%s", "");
        if (s_info != NULL) {
            s_info(&words);
        }
        p3a_ui_screen_show(P3A_SCREEN_INFO, &words);
        break;
    case UI_ACTION_CLOSE_SCREEN:
        p3a_ui_screen_hide(screen);
        break;
    case UI_ACTION_RESUME:
        play_scheduler_set_paused(false);
        break;
    case UI_ACTION_LIKE:
    case UI_ACTION_UNLIKE:
        // What these do depends on where the artwork is from; it arrives
        // with the sources (Phase 4). Recognised and counted until then.
        break;
    default:
        break;
    }
}

static void touch_task(void *arg)
{
    (void)arg;
    static ui_gesture_state_t gestures;
    ui_gesture_init(&gestures, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int64_t now_ms = esp_timer_get_time() / 1000;
        ui_touch_t touch;
        sample(&touch, now_ms);
        const int rotation = (int)display_renderer_get_rotation();
        const ui_gesture_t gesture = ui_gesture_feed(&gestures, &touch, now_ms, rotation);
        if (gesture.kind == UI_GESTURE_NONE) {
            continue;
        }
        p3a_ui_state_t state;
        p3a_ui_get_state(&state);
        const int32_t view_width = (rotation == 90 || rotation == 270) ? P3A_DISPLAY_HEIGHT : P3A_DISPLAY_WIDTH;
        const ui_action_t action = ui_route(state.screen, play_scheduler_is_paused(), &gesture, view_width);
        ESP_LOGI(TAG, "%s on %s: %s", ui_gesture_name(gesture.kind), p3a_ui_screen_name(state.screen),
                 ui_action_name(action));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.gestures++;
        s_status.last_gesture = ui_gesture_name(gesture.kind);
        s_status.last_action = ui_action_name(action);
        xSemaphoreGive(s_lock);
        act(action, state.screen);
    }
}

esp_err_t ui_touch_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_status.last_gesture = "none";
    s_status.last_action = "none";
    const p3a_task_config_t config = P3A_TASK_CONFIG("ui_touch", touch_task, NULL, 4096, P3A_TASK_TOUCH);
    return p3a_task_create(&config, NULL);
}
