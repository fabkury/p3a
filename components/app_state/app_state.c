#include "app_state.h"
#include "esp_log.h"

static const char *TAG = "STATE";

static app_state_t g_state = STATE_READY;
static SemaphoreHandle_t g_mutex;

void app_state_init(void) {
    g_mutex = xSemaphoreCreateMutex();
    configASSERT(g_mutex);
    g_state = STATE_READY;
    ESP_LOGI(TAG, "Initialized, state=READY");
}

static void set_state(app_state_t s) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    app_state_t old = g_state;
    g_state = s;
    xSemaphoreGive(g_mutex);
    if (old != s) {
        ESP_LOGI(TAG, "state transition: %s -> %s", app_state_str(old), app_state_str(s));
    }
}

app_state_t app_state_get(void) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    app_state_t s = g_state;
    xSemaphoreGive(g_mutex);
    return s;
}

const char* app_state_str(app_state_t s) {
    switch(s) {
        case STATE_READY:      return "READY";
        case STATE_PROCESSING: return "PROCESSING";
        case STATE_ERROR:      return "ERROR";
        default:               return "UNKNOWN";
    }
}

void app_state_enter_ready(void) {
    set_state(STATE_READY);
}

void app_state_enter_processing(void) {
    set_state(STATE_PROCESSING);
}

void app_state_enter_error(void) {
    set_state(STATE_ERROR);
}

