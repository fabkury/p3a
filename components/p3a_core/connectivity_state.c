// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "connectivity_state.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

static const char *TAG = "conn_state";

// ============================================================================
// Configuration
// ============================================================================

// Internet check interval when in NO_INTERNET state (60 seconds)
#define INTERNET_CHECK_INTERVAL_MS 60000

// DNS lookup timeout for internet check (5 seconds)
#define DNS_LOOKUP_TIMEOUT_MS 5000

// MQTT reconnection backoff parameters
#define MQTT_BACKOFF_MIN_MS 5000
#define MQTT_BACKOFF_MAX_MS 300000
#define MQTT_BACKOFF_JITTER_PERCENT 25

// Maximum number of callbacks
#define MAX_CALLBACKS 8

// ============================================================================
// State Messages
// ============================================================================

static const char *s_short_messages[] = {
    [CONN_STATE_NO_WIFI] = "No Wi-Fi",
    [CONN_STATE_NO_INTERNET] = "No Internet",
    [CONN_STATE_NO_REGISTRATION] = "Not Registered",
    [CONN_STATE_NO_MQTT] = "Connecting...",
    [CONN_STATE_ONLINE] = "Online",
};

static const char *s_detail_messages[] = {
    [CONN_STATE_NO_WIFI] = "Connect to Wi-Fi network",
    [CONN_STATE_NO_INTERNET] = "Wi-Fi connected but no internet access",
    [CONN_STATE_NO_REGISTRATION] = "Long-press to register with Makapix Club",
    [CONN_STATE_NO_MQTT] = "Connecting to Makapix Cloud",
    [CONN_STATE_ONLINE] = "Connected to Makapix Club",
};

// ============================================================================
// Internal State
// ============================================================================

typedef struct {
    connectivity_state_cb_t cb;
    void *user_ctx;
} callback_entry_t;

static struct {
    bool initialized;
    connectivity_state_t current_state;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t event_group;
    TimerHandle_t internet_check_timer;

    // Callbacks
    callback_entry_t callbacks[MAX_CALLBACKS];
    size_t callback_count;

    // Internet check state
    time_t last_internet_check;
    bool internet_check_in_progress;

    // MQTT backoff
    uint32_t mqtt_backoff_ms;

    // Registration status (cached)
    bool has_registration;
} s_state = {
    .current_state = CONN_STATE_NO_WIFI,
    .mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS,
};

// Event group bits
#define EG_BIT_ONLINE       (1 << 0)
#define EG_BIT_INTERNET     (1 << 1)
#define EG_BIT_WIFI         (1 << 2)

// ============================================================================
// Internal Functions
// ============================================================================

/**
 * @brief Update event group bits based on current state
 */
static void update_event_group(void)
{
    if (!s_state.event_group) return;

    EventBits_t bits = 0;

    switch (s_state.current_state) {
        case CONN_STATE_ONLINE:
            bits = EG_BIT_ONLINE | EG_BIT_INTERNET | EG_BIT_WIFI;
            break;
        case CONN_STATE_NO_MQTT:
        case CONN_STATE_NO_REGISTRATION:
            bits = EG_BIT_INTERNET | EG_BIT_WIFI;
            break;
        case CONN_STATE_NO_INTERNET:
            bits = EG_BIT_WIFI;
            break;
        case CONN_STATE_NO_WIFI:
        default:
            bits = 0;
            break;
    }

    // Clear all bits and set new ones
    xEventGroupClearBits(s_state.event_group, EG_BIT_ONLINE | EG_BIT_INTERNET | EG_BIT_WIFI);
    if (bits) {
        xEventGroupSetBits(s_state.event_group, bits);
    }
}

/**
 * @brief Set state and notify callbacks
 */
static void set_state(connectivity_state_t new_state)
{
    connectivity_state_t old_state = s_state.current_state;
    if (old_state == new_state) return;

    ESP_LOGI(TAG, "State: %s -> %s",
             s_short_messages[old_state], s_short_messages[new_state]);

    s_state.current_state = new_state;
    update_event_group();

    // Notify callbacks (still under mutex - keep callbacks fast)
    for (size_t i = 0; i < s_state.callback_count; i++) {
        if (s_state.callbacks[i].cb) {
            s_state.callbacks[i].cb(old_state, new_state, s_state.callbacks[i].user_ctx);
        }
    }
}

/**
 * @brief Timer callback for periodic internet checks
 */
static void internet_check_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (!s_state.initialized) return;

    // Only check if we're in NO_INTERNET state
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    bool should_check = (s_state.current_state == CONN_STATE_NO_INTERNET);
    xSemaphoreGive(s_state.mutex);

    if (should_check) {
        connectivity_state_check_internet();
    }
}

/**
 * @brief Check if Makapix registration exists
 *
 * Uses weak symbol to avoid hard dependency on makapix component.
 */
static bool check_registration(void)
{
    extern bool makapix_store_has_player_key(void) __attribute__((weak));
    if (makapix_store_has_player_key) {
        return makapix_store_has_player_key();
    }
    return false;
}

// ============================================================================
// Public API - Initialization
// ============================================================================

esp_err_t connectivity_state_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_state.event_group = xEventGroupCreate();
    if (!s_state.event_group) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_state.internet_check_timer = xTimerCreate(
        "inet_check",
        pdMS_TO_TICKS(INTERNET_CHECK_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        NULL,
        internet_check_timer_cb
    );

    if (!s_state.internet_check_timer) {
        vEventGroupDelete(s_state.event_group);
        vSemaphoreDelete(s_state.mutex);
        s_state.event_group = NULL;
        s_state.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_state.current_state = CONN_STATE_NO_WIFI;
    s_state.callback_count = 0;
    s_state.last_internet_check = 0;
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    s_state.has_registration = check_registration();
    s_state.initialized = true;

    ESP_LOGI(TAG, "Connectivity state initialized (registration=%d)", s_state.has_registration);
    return ESP_OK;
}

void connectivity_state_deinit(void)
{
    if (!s_state.initialized) return;

    if (s_state.internet_check_timer) {
        xTimerStop(s_state.internet_check_timer, portMAX_DELAY);
        xTimerDelete(s_state.internet_check_timer, portMAX_DELAY);
        s_state.internet_check_timer = NULL;
    }

    if (s_state.event_group) {
        vEventGroupDelete(s_state.event_group);
        s_state.event_group = NULL;
    }

    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    s_state.initialized = false;
    ESP_LOGI(TAG, "Connectivity state deinitialized");
}

// ============================================================================
// Public API - State Access
// ============================================================================

connectivity_state_t connectivity_state_get(void)
{
    if (!s_state.initialized) {
        return CONN_STATE_NO_WIFI;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    connectivity_state_t state = s_state.current_state;
    xSemaphoreGive(s_state.mutex);

    return state;
}

const char *connectivity_state_get_message(void)
{
    connectivity_state_t state = connectivity_state_get();
    if (state < sizeof(s_short_messages) / sizeof(s_short_messages[0])) {
        return s_short_messages[state];
    }
    return "Unknown";
}

const char *connectivity_state_get_detail(void)
{
    connectivity_state_t state = connectivity_state_get();
    if (state < sizeof(s_detail_messages) / sizeof(s_detail_messages[0])) {
        return s_detail_messages[state];
    }
    return "Unknown state";
}

bool connectivity_state_is_online(void)
{
    return connectivity_state_get() == CONN_STATE_ONLINE;
}

bool connectivity_state_has_internet(void)
{
    connectivity_state_t state = connectivity_state_get();
    return state >= CONN_STATE_NO_REGISTRATION;
}

bool connectivity_state_has_wifi(void)
{
    connectivity_state_t state = connectivity_state_get();
    return state >= CONN_STATE_NO_INTERNET;
}

// ============================================================================
// Public API - Waiting
// ============================================================================

esp_err_t connectivity_state_wait_for_online(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.event_group,
        EG_BIT_ONLINE,
        pdFALSE,  // Don't clear
        pdTRUE,   // Wait for all bits
        timeout_ms
    );

    return (bits & EG_BIT_ONLINE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t connectivity_state_wait_for_internet(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.event_group,
        EG_BIT_INTERNET,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_INTERNET) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t connectivity_state_wait_for_wifi(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.event_group,
        EG_BIT_WIFI,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_WIFI) ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ============================================================================
// Public API - Callbacks
// ============================================================================

esp_err_t connectivity_state_register_callback(connectivity_state_cb_t cb, void *user_ctx)
{
    if (!s_state.initialized || !cb) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (s_state.callback_count >= MAX_CALLBACKS) {
        xSemaphoreGive(s_state.mutex);
        return ESP_ERR_NO_MEM;
    }

    s_state.callbacks[s_state.callback_count].cb = cb;
    s_state.callbacks[s_state.callback_count].user_ctx = user_ctx;
    s_state.callback_count++;

    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

void connectivity_state_unregister_callback(connectivity_state_cb_t cb)
{
    if (!s_state.initialized || !cb) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_state.callback_count; i++) {
        if (s_state.callbacks[i].cb == cb) {
            // Shift remaining entries
            for (size_t j = i; j < s_state.callback_count - 1; j++) {
                s_state.callbacks[j] = s_state.callbacks[j + 1];
            }
            s_state.callback_count--;
            break;
        }
    }

    xSemaphoreGive(s_state.mutex);
}

// ============================================================================
// Public API - Event Handlers
// ============================================================================

void connectivity_state_on_wifi_connected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Transition to NO_INTERNET (will check internet and advance)
    set_state(CONN_STATE_NO_INTERNET);

    // Start periodic internet check timer
    if (s_state.internet_check_timer) {
        xTimerStart(s_state.internet_check_timer, 0);
    }

    xSemaphoreGive(s_state.mutex);

    // Trigger immediate internet check
    connectivity_state_check_internet();
}

void connectivity_state_on_wifi_disconnected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Stop internet check timer
    if (s_state.internet_check_timer) {
        xTimerStop(s_state.internet_check_timer, 0);
    }

    // Transition to NO_WIFI from any state
    set_state(CONN_STATE_NO_WIFI);

    // Reset MQTT backoff
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;

    xSemaphoreGive(s_state.mutex);
}

void connectivity_state_on_mqtt_connected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Transition to ONLINE
    set_state(CONN_STATE_ONLINE);

    // Reset MQTT backoff on successful connection
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;

    xSemaphoreGive(s_state.mutex);
}

void connectivity_state_on_mqtt_disconnected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Only transition if we were online or trying to connect
    if (s_state.current_state >= CONN_STATE_NO_MQTT) {
        // Check if still registered
        s_state.has_registration = check_registration();

        if (s_state.has_registration) {
            set_state(CONN_STATE_NO_MQTT);
        } else {
            set_state(CONN_STATE_NO_REGISTRATION);
        }

        // Increase backoff with jitter
        s_state.mqtt_backoff_ms = s_state.mqtt_backoff_ms * 2;
        if (s_state.mqtt_backoff_ms > MQTT_BACKOFF_MAX_MS) {
            s_state.mqtt_backoff_ms = MQTT_BACKOFF_MAX_MS;
        }

        // Add jitter (±25%)
        uint32_t jitter = (s_state.mqtt_backoff_ms * MQTT_BACKOFF_JITTER_PERCENT) / 100;
        uint32_t rand_val = esp_random() % (jitter * 2);
        s_state.mqtt_backoff_ms = s_state.mqtt_backoff_ms - jitter + rand_val;
    }

    xSemaphoreGive(s_state.mutex);
}

void connectivity_state_on_registration_changed(bool has_registration)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    s_state.has_registration = has_registration;

    // Update state based on registration status
    if (s_state.current_state == CONN_STATE_NO_REGISTRATION && has_registration) {
        // Registration completed, try to connect MQTT
        set_state(CONN_STATE_NO_MQTT);
    } else if (s_state.current_state >= CONN_STATE_NO_MQTT && !has_registration) {
        // Registration invalidated
        set_state(CONN_STATE_NO_REGISTRATION);
    }

    xSemaphoreGive(s_state.mutex);
}

// ============================================================================
// Public API - Internet Check
// ============================================================================

/**
 * @brief DNS callback for internet check
 */
static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    bool *result = (bool *)arg;
    *result = (ipaddr != NULL);
}

bool connectivity_state_check_internet(void)
{
    if (!s_state.initialized) return false;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    // Prevent concurrent checks
    if (s_state.internet_check_in_progress) {
        xSemaphoreGive(s_state.mutex);
        return s_state.current_state >= CONN_STATE_NO_REGISTRATION;
    }
    s_state.internet_check_in_progress = true;

    xSemaphoreGive(s_state.mutex);

    // Perform DNS lookup for example.com
    ESP_LOGD(TAG, "Checking internet via DNS lookup...");

    ip_addr_t addr;
    volatile bool dns_done = false;
    volatile bool dns_success = false;

    err_t err = dns_gethostbyname("example.com", &addr,
                                   (dns_found_callback)dns_callback,
                                   (void *)&dns_success);

    if (err == ERR_OK) {
        // DNS was cached
        dns_success = true;
        dns_done = true;
    } else if (err == ERR_INPROGRESS) {
        // Wait for callback
        TickType_t start = xTaskGetTickCount();
        while (!dns_done &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(DNS_LOOKUP_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            // Check if callback was called (dns_success would be set)
            if (dns_success) {
                dns_done = true;
                break;
            }
        }
    }

    // Alternative: simple connectivity check via esp_netif
    // If DNS fails but we have a valid IP, assume internet is available
    if (!dns_success) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                if (ip_info.ip.addr != 0 && ip_info.gw.addr != 0) {
                    // We have a valid IP and gateway - assume internet works
                    dns_success = true;
                    ESP_LOGD(TAG, "DNS failed but have IP - assuming internet OK");
                }
            }
        }
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.internet_check_in_progress = false;

    if (dns_success) {
        s_state.last_internet_check = time(NULL);

        // Internet is available - advance state if needed
        if (s_state.current_state == CONN_STATE_NO_INTERNET) {
            s_state.has_registration = check_registration();
            if (s_state.has_registration) {
                set_state(CONN_STATE_NO_MQTT);
            } else {
                set_state(CONN_STATE_NO_REGISTRATION);
            }
        }

        ESP_LOGI(TAG, "Internet check: OK");
    } else {
        // Internet not available
        if (s_state.current_state > CONN_STATE_NO_INTERNET) {
            set_state(CONN_STATE_NO_INTERNET);
        }
        ESP_LOGW(TAG, "Internet check: FAILED");
    }

    bool result = (s_state.current_state >= CONN_STATE_NO_REGISTRATION);
    xSemaphoreGive(s_state.mutex);

    return result;
}

uint32_t connectivity_state_get_last_internet_check_age(void)
{
    if (!s_state.initialized || s_state.last_internet_check == 0) {
        return UINT32_MAX;
    }

    time_t now = time(NULL);
    if (now < s_state.last_internet_check) {
        return 0;  // Clock went backwards
    }

    return (uint32_t)(now - s_state.last_internet_check);
}
