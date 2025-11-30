#include "makapix.h"
#include "makapix_store.h"
#include "makapix_provision.h"
#include "makapix_mqtt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "makapix";

static makapix_state_t s_state = MAKAPIX_STATE_IDLE;
static int32_t s_current_post_id = 0;
static char s_registration_code[8] = {0};  // 6 chars + null + padding to avoid warning
static char s_registration_expires[64] = {0};  // Extra space to avoid truncation warning
static TimerHandle_t s_status_timer = NULL;
static bool s_provisioning_cancelled = false;  // Flag to prevent race condition

#define STATUS_PUBLISH_INTERVAL_MS (30000) // 30 seconds

/**
 * @brief Timer callback for periodic status publishing
 */
static void status_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (makapix_mqtt_is_connected()) {
        makapix_mqtt_publish_status(makapix_get_current_post_id());
    }
}

/**
 * @brief Provisioning task
 */
static void provisioning_task(void *pvParameters)
{
    makapix_provision_result_t result;
    esp_err_t err = makapix_provision_request(&result);

    // Check if provisioning was cancelled while we were waiting
    if (s_provisioning_cancelled) {
        ESP_LOGI(TAG, "Provisioning was cancelled, aborting");
        s_provisioning_cancelled = false;  // Reset flag
        vTaskDelete(NULL);
        return;
    }

    if (err == ESP_OK) {
        // Double-check cancellation before saving (race condition protection)
        if (s_provisioning_cancelled) {
            ESP_LOGI(TAG, "Provisioning was cancelled after request completed, aborting");
            s_provisioning_cancelled = false;
            vTaskDelete(NULL);
            return;
        }

        // Save credentials
        err = makapix_store_save_credentials(result.player_key, result.mqtt_host, result.mqtt_port);
        if (err == ESP_OK) {
            // Final check before updating state
            if (!s_provisioning_cancelled) {
                // Store registration code for display
                snprintf(s_registration_code, sizeof(s_registration_code), "%s", result.registration_code);
                snprintf(s_registration_expires, sizeof(s_registration_expires), "%s", result.expires_at);
                
                s_state = MAKAPIX_STATE_SHOW_CODE;
                ESP_LOGI(TAG, "Provisioning successful, registration code: %s", s_registration_code);
            } else {
                ESP_LOGI(TAG, "Provisioning was cancelled, discarding results");
                s_provisioning_cancelled = false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
            s_state = MAKAPIX_STATE_IDLE;
        }
    } else {
        ESP_LOGE(TAG, "Provisioning failed: %s", esp_err_to_name(err));
        if (!s_provisioning_cancelled) {
            s_state = MAKAPIX_STATE_IDLE;
        }
    }

    s_provisioning_cancelled = false;  // Reset flag
    vTaskDelete(NULL);
}

/**
 * @brief MQTT reconnection task
 */
static void mqtt_reconnect_task(void *pvParameters)
{
    char player_key[37];
    char mqtt_host[64];
    uint16_t mqtt_port;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // Wait 5 seconds before retry

        if (makapix_store_get_player_key(player_key, sizeof(player_key)) == ESP_OK &&
            makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host)) == ESP_OK &&
            makapix_store_get_mqtt_port(&mqtt_port) == ESP_OK) {

            if (!makapix_mqtt_is_connected()) {
                ESP_LOGI(TAG, "Attempting MQTT reconnection...");
                s_state = MAKAPIX_STATE_CONNECTING;
                
                esp_err_t err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port);
                if (err == ESP_OK) {
                    err = makapix_mqtt_connect();
                    if (err == ESP_OK) {
                        s_state = MAKAPIX_STATE_CONNECTED;
                        ESP_LOGI(TAG, "MQTT reconnected successfully");
                    } else {
                        s_state = MAKAPIX_STATE_DISCONNECTED;
                        ESP_LOGW(TAG, "MQTT connection failed: %s", esp_err_to_name(err));
                    }
                } else {
                    s_state = MAKAPIX_STATE_DISCONNECTED;
                    ESP_LOGW(TAG, "MQTT init failed: %s", esp_err_to_name(err));
                }
            } else {
                // Already connected, exit task
                break;
            }
        } else {
            // No credentials, exit task
            break;
        }
    }

    vTaskDelete(NULL);
}

esp_err_t makapix_init(void)
{
    makapix_store_init();

    if (makapix_store_has_player_key()) {
        ESP_LOGI(TAG, "Found stored player_key, will connect after WiFi");
        s_state = MAKAPIX_STATE_IDLE; // Will transition to CONNECTING when WiFi connects
    } else {
        ESP_LOGI(TAG, "No player_key found, waiting for provisioning gesture");
        s_state = MAKAPIX_STATE_IDLE;
    }

    s_current_post_id = 0;
    memset(s_registration_code, 0, sizeof(s_registration_code));
    memset(s_registration_expires, 0, sizeof(s_registration_expires));

    return ESP_OK;
}

makapix_state_t makapix_get_state(void)
{
    return s_state;
}

esp_err_t makapix_start_provisioning(void)
{
    // If already in provisioning/show_code, cancel first
    if (s_state == MAKAPIX_STATE_PROVISIONING || s_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGI(TAG, "Cancelling existing provisioning before starting new one");
        makapix_cancel_provisioning();
        // Small delay to ensure cleanup completes
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Starting provisioning...");
    s_state = MAKAPIX_STATE_PROVISIONING;
    s_provisioning_cancelled = false;  // Reset cancellation flag

    // Start provisioning task
    BaseType_t ret = xTaskCreate(provisioning_task, "makapix_prov", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create provisioning task");
        s_state = MAKAPIX_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void makapix_cancel_provisioning(void)
{
    if (s_state == MAKAPIX_STATE_PROVISIONING || s_state == MAKAPIX_STATE_SHOW_CODE) {
        ESP_LOGI(TAG, "Cancelling provisioning");
        s_provisioning_cancelled = true;  // Set flag to abort provisioning task
        s_state = MAKAPIX_STATE_IDLE;
        memset(s_registration_code, 0, sizeof(s_registration_code));
        memset(s_registration_expires, 0, sizeof(s_registration_expires));
    }
}

int32_t makapix_get_current_post_id(void)
{
    return s_current_post_id;
}

void makapix_set_current_post_id(int32_t post_id)
{
    s_current_post_id = post_id;
}

esp_err_t makapix_connect_if_registered(void)
{
    if (s_state == MAKAPIX_STATE_CONNECTED || s_state == MAKAPIX_STATE_CONNECTING) {
        ESP_LOGW(TAG, "MQTT already connected or connecting");
        return ESP_OK;
    }

    char player_key[37];
    char mqtt_host[64];
    uint16_t mqtt_port;

    esp_err_t err = makapix_store_get_player_key(player_key, sizeof(player_key));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No player_key stored");
        return ESP_ERR_NOT_FOUND;
    }

    err = makapix_store_get_mqtt_host(mqtt_host, sizeof(mqtt_host));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No MQTT host stored");
        return err;
    }

    err = makapix_store_get_mqtt_port(&mqtt_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No MQTT port stored");
        return err;
    }

    ESP_LOGI(TAG, "Connecting to MQTT broker: %s:%d", mqtt_host, mqtt_port);
    s_state = MAKAPIX_STATE_CONNECTING;

    err = makapix_mqtt_init(player_key, mqtt_host, mqtt_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT: %s", esp_err_to_name(err));
        s_state = MAKAPIX_STATE_DISCONNECTED;
        return err;
    }

    err = makapix_mqtt_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect MQTT: %s", esp_err_to_name(err));
        s_state = MAKAPIX_STATE_DISCONNECTED;
        // Start reconnection task
        xTaskCreate(mqtt_reconnect_task, "mqtt_reconn", 4096, NULL, 5, NULL);
        return err;
    }

    s_state = MAKAPIX_STATE_CONNECTED;
    ESP_LOGI(TAG, "MQTT connected successfully");

    // Publish initial status
    makapix_mqtt_publish_status(makapix_get_current_post_id());

    // Create periodic status timer
    if (!s_status_timer) {
        s_status_timer = xTimerCreate("status_timer", pdMS_TO_TICKS(STATUS_PUBLISH_INTERVAL_MS),
                                      pdTRUE, NULL, status_timer_callback);
        if (s_status_timer) {
            xTimerStart(s_status_timer, 0);
        }
    }

    return ESP_OK;
}

esp_err_t makapix_get_registration_code(char *out_code, size_t max_len)
{
    if (!out_code || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(s_registration_code) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(out_code, s_registration_code, max_len - 1);
    out_code[max_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t makapix_get_registration_expires(char *out_expires, size_t max_len)
{
    if (!out_expires || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(s_registration_expires) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(out_expires, s_registration_expires, max_len - 1);
    out_expires[max_len - 1] = '\0';
    return ESP_OK;
}

