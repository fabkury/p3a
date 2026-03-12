// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/**
 * @file p3a_state_connectivity.c
 * @brief Connectivity tracking: Wi-Fi/MQTT/internet state, DNS checks, backoff
 */

#include "p3a_state_internal.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "event_bus.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include <time.h>

static const char *TAG = "p3a_state_conn";

static const char *s_connectivity_short_messages[] = {
    [P3A_CONNECTIVITY_NO_WIFI] = "No Wi-Fi",
    [P3A_CONNECTIVITY_NO_INTERNET] = "No Internet",
    [P3A_CONNECTIVITY_NO_REGISTRATION] = "Not Registered",
    [P3A_CONNECTIVITY_NO_MQTT] = "Connecting...",
    [P3A_CONNECTIVITY_ONLINE] = "Online",
};

static const char *s_connectivity_detail_messages[] = {
    [P3A_CONNECTIVITY_NO_WIFI] = "Connect to Wi-Fi network",
    [P3A_CONNECTIVITY_NO_INTERNET] = "Wi-Fi connected but no internet access",
    [P3A_CONNECTIVITY_NO_REGISTRATION] = "Long-press to register with Makapix Club",
    [P3A_CONNECTIVITY_NO_MQTT] = "Connecting to Makapix Cloud",
    [P3A_CONNECTIVITY_ONLINE] = "Connected to Makapix Club",
};

// Connectivity configuration
#define INTERNET_CHECK_INTERVAL_MS 60000
#define DNS_LOOKUP_TIMEOUT_MS 5000
#define MQTT_BACKOFF_MIN_MS 5000
#define MQTT_BACKOFF_MAX_MS 300000
#define MQTT_BACKOFF_JITTER_PERCENT 25

// Event group bits for connectivity
#define EG_BIT_ONLINE       (1 << 0)
#define EG_BIT_INTERNET     (1 << 1)
#define EG_BIT_WIFI         (1 << 2)

// ============================================================================
// Internal Helpers
// ============================================================================

static void update_connectivity_event_group_locked(void)
{
    if (!s_state.connectivity_event_group) {
        return;
    }

    EventBits_t bits = 0;
    switch (s_state.connectivity) {
        case P3A_CONNECTIVITY_ONLINE:
            bits = EG_BIT_ONLINE | EG_BIT_INTERNET | EG_BIT_WIFI;
            break;
        case P3A_CONNECTIVITY_NO_MQTT:
        case P3A_CONNECTIVITY_NO_REGISTRATION:
            bits = EG_BIT_INTERNET | EG_BIT_WIFI;
            break;
        case P3A_CONNECTIVITY_NO_INTERNET:
            bits = EG_BIT_WIFI;
            break;
        case P3A_CONNECTIVITY_NO_WIFI:
        default:
            bits = 0;
            break;
    }

    xEventGroupClearBits(s_state.connectivity_event_group,
                         EG_BIT_ONLINE | EG_BIT_INTERNET | EG_BIT_WIFI);
    if (bits) {
        xEventGroupSetBits(s_state.connectivity_event_group, bits);
    }
}

static void set_connectivity_locked(p3a_connectivity_level_t new_level)
{
    if (s_state.connectivity == new_level) {
        return;
    }

    ESP_LOGI(TAG, "Connectivity: %s -> %s",
             s_connectivity_short_messages[s_state.connectivity],
             s_connectivity_short_messages[new_level]);

    s_state.connectivity = new_level;
    update_connectivity_event_group_locked();
}

static bool check_registration(void)
{
    extern bool makapix_store_has_player_key(void) __attribute__((weak));
    if (makapix_store_has_player_key) {
        return makapix_store_has_player_key();
    }
    return false;
}

static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    bool *result = (bool *)arg;
    *result = (ipaddr != NULL);
}

static void internet_check_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (!s_state.initialized) {
        return;
    }

    bool should_check = false;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    should_check = (s_state.connectivity == P3A_CONNECTIVITY_NO_INTERNET);
    xSemaphoreGive(s_state.mutex);

    if (should_check) {
        event_bus_emit_simple(P3A_EVENT_INTERNET_CHECK);
    }
}

// ============================================================================
// Public Functions
// ============================================================================

esp_err_t p3a_state_connectivity_init(void)
{
    if (s_state.connectivity_event_group || s_state.internet_check_timer) {
        return ESP_OK;
    }

    s_state.connectivity_event_group = xEventGroupCreate();
    if (!s_state.connectivity_event_group) {
        return ESP_ERR_NO_MEM;
    }

    s_state.internet_check_timer = xTimerCreate(
        "inet_check",
        pdMS_TO_TICKS(INTERNET_CHECK_INTERVAL_MS),
        pdTRUE,
        NULL,
        internet_check_timer_cb
    );
    if (!s_state.internet_check_timer) {
        vEventGroupDelete(s_state.connectivity_event_group);
        s_state.connectivity_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.connectivity = P3A_CONNECTIVITY_NO_WIFI;
    s_state.last_internet_check = 0;
    s_state.internet_check_in_progress = false;
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    s_state.has_registration = check_registration();
    update_connectivity_event_group_locked();
    xSemaphoreGive(s_state.mutex);

    ESP_LOGI(TAG, "Connectivity initialized (registration=%d)", s_state.has_registration);
    return ESP_OK;
}

void p3a_state_connectivity_deinit(void)
{
    if (s_state.internet_check_timer) {
        xTimerStop(s_state.internet_check_timer, portMAX_DELAY);
        xTimerDelete(s_state.internet_check_timer, portMAX_DELAY);
        s_state.internet_check_timer = NULL;
    }
    if (s_state.connectivity_event_group) {
        vEventGroupDelete(s_state.connectivity_event_group);
        s_state.connectivity_event_group = NULL;
    }
}

void p3a_state_on_wifi_connected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    set_connectivity_locked(P3A_CONNECTIVITY_NO_INTERNET);
    if (s_state.internet_check_timer) {
        xTimerStart(s_state.internet_check_timer, 0);
    }
    xSemaphoreGive(s_state.mutex);

    p3a_state_check_internet();
}

void p3a_state_on_wifi_disconnected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    if (s_state.internet_check_timer) {
        xTimerStop(s_state.internet_check_timer, 0);
    }
    set_connectivity_locked(P3A_CONNECTIVITY_NO_WIFI);
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    xSemaphoreGive(s_state.mutex);
}

void p3a_state_on_mqtt_connected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    set_connectivity_locked(P3A_CONNECTIVITY_ONLINE);
    s_state.mqtt_backoff_ms = MQTT_BACKOFF_MIN_MS;
    xSemaphoreGive(s_state.mutex);
}

void p3a_state_on_mqtt_disconnected(void)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);

    if (s_state.connectivity >= P3A_CONNECTIVITY_NO_MQTT) {
        s_state.has_registration = check_registration();
        if (s_state.has_registration) {
            set_connectivity_locked(P3A_CONNECTIVITY_NO_MQTT);
        } else {
            set_connectivity_locked(P3A_CONNECTIVITY_NO_REGISTRATION);
        }

        s_state.mqtt_backoff_ms = s_state.mqtt_backoff_ms * 2;
        if (s_state.mqtt_backoff_ms > MQTT_BACKOFF_MAX_MS) {
            s_state.mqtt_backoff_ms = MQTT_BACKOFF_MAX_MS;
        }

        uint32_t jitter = (s_state.mqtt_backoff_ms * MQTT_BACKOFF_JITTER_PERCENT) / 100;
        uint32_t rand_val = esp_random() % (jitter * 2);
        s_state.mqtt_backoff_ms = s_state.mqtt_backoff_ms - jitter + rand_val;
    }

    xSemaphoreGive(s_state.mutex);
}

void p3a_state_on_registration_changed(bool has_registration)
{
    if (!s_state.initialized) return;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.has_registration = has_registration;

    if (s_state.connectivity == P3A_CONNECTIVITY_NO_REGISTRATION && has_registration) {
        set_connectivity_locked(P3A_CONNECTIVITY_NO_MQTT);
    } else if (s_state.connectivity >= P3A_CONNECTIVITY_NO_MQTT && !has_registration) {
        set_connectivity_locked(P3A_CONNECTIVITY_NO_REGISTRATION);
    }

    xSemaphoreGive(s_state.mutex);
}

bool p3a_state_check_internet(void)
{
    if (!s_state.initialized) return false;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    if (s_state.internet_check_in_progress) {
        xSemaphoreGive(s_state.mutex);
        return s_state.connectivity >= P3A_CONNECTIVITY_NO_REGISTRATION;
    }
    s_state.internet_check_in_progress = true;
    xSemaphoreGive(s_state.mutex);

    ESP_LOGD(TAG, "Checking internet via DNS lookup...");
    ip_addr_t addr;
    volatile bool dns_done = false;
    volatile bool dns_success = false;

    err_t err = dns_gethostbyname("example.com", &addr,
                                   (dns_found_callback)dns_callback,
                                   (void *)&dns_success);

    if (err == ERR_OK) {
        dns_success = true;
        dns_done = true;
    } else if (err == ERR_INPROGRESS) {
        TickType_t start = xTaskGetTickCount();
        while (!dns_done &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(DNS_LOOKUP_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (dns_success) {
                dns_done = true;
                break;
            }
        }
    }

    if (!dns_success) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                if (ip_info.ip.addr != 0 && ip_info.gw.addr != 0) {
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
        if (s_state.connectivity == P3A_CONNECTIVITY_NO_INTERNET) {
            s_state.has_registration = check_registration();
            if (s_state.has_registration) {
                set_connectivity_locked(P3A_CONNECTIVITY_NO_MQTT);
            } else {
                set_connectivity_locked(P3A_CONNECTIVITY_NO_REGISTRATION);
            }
        }
        ESP_LOGI(TAG, "Internet check: OK");
    } else {
        if (s_state.connectivity > P3A_CONNECTIVITY_NO_INTERNET) {
            set_connectivity_locked(P3A_CONNECTIVITY_NO_INTERNET);
        }
        ESP_LOGW(TAG, "Internet check: FAILED");
    }

    bool result = (s_state.connectivity >= P3A_CONNECTIVITY_NO_REGISTRATION);
    xSemaphoreGive(s_state.mutex);
    return result;
}

uint32_t p3a_state_get_last_internet_check_age(void)
{
    if (!s_state.initialized || s_state.last_internet_check == 0) {
        return UINT32_MAX;
    }

    time_t now = time(NULL);
    if (now < s_state.last_internet_check) {
        return 0;
    }

    return (uint32_t)(now - s_state.last_internet_check);
}

esp_err_t p3a_state_wait_for_online(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.connectivity_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.connectivity_event_group,
        EG_BIT_ONLINE,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_ONLINE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t p3a_state_wait_for_internet(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.connectivity_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.connectivity_event_group,
        EG_BIT_INTERNET,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_INTERNET) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t p3a_state_wait_for_wifi(TickType_t timeout_ms)
{
    if (!s_state.initialized || !s_state.connectivity_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_state.connectivity_event_group,
        EG_BIT_WIFI,
        pdFALSE,
        pdTRUE,
        timeout_ms
    );

    return (bits & EG_BIT_WIFI) ? ESP_OK : ESP_ERR_TIMEOUT;
}

p3a_connectivity_level_t p3a_state_get_connectivity(void)
{
    if (!s_state.initialized) return P3A_CONNECTIVITY_NO_WIFI;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    p3a_connectivity_level_t level = s_state.connectivity;
    xSemaphoreGive(s_state.mutex);

    return level;
}

const char *p3a_state_get_connectivity_message(void)
{
    p3a_connectivity_level_t level = p3a_state_get_connectivity();
    if (level < (sizeof(s_connectivity_short_messages) / sizeof(s_connectivity_short_messages[0]))) {
        return s_connectivity_short_messages[level];
    }
    return "Unknown";
}

const char *p3a_state_get_connectivity_detail(void)
{
    p3a_connectivity_level_t level = p3a_state_get_connectivity();
    if (level < (sizeof(s_connectivity_detail_messages) / sizeof(s_connectivity_detail_messages[0]))) {
        return s_connectivity_detail_messages[level];
    }
    return "Unknown state";
}

bool p3a_state_has_wifi(void)
{
    return p3a_state_get_connectivity() >= P3A_CONNECTIVITY_NO_INTERNET;
}

bool p3a_state_has_internet(void)
{
    return p3a_state_get_connectivity() >= P3A_CONNECTIVITY_NO_REGISTRATION;
}

bool p3a_state_is_online(void)
{
    return p3a_state_get_connectivity() == P3A_CONNECTIVITY_ONLINE;
}
