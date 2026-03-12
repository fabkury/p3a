// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#pragma once

#include "esp_wifi.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// ESP-IDF default interface key for WiFi STA (used by esp_netif_create_default_wifi_sta)
#define WIFI_STA_NETIF_KEY "WIFI_STA_DEF"

#define MAX_SSID_LEN     32
#define MAX_PASSWORD_LEN 64

#define NVS_NAMESPACE    "wifi_config"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASSWORD "password"

// Shared state (defined in app_wifi.c)
extern EventGroupHandle_t s_wifi_event_group;
extern int s_retry_num;
extern bool s_initial_connection_done;
extern bool s_services_initialized;
extern int s_consecutive_wifi_errors;
extern const int s_max_consecutive_wifi_errors;
extern bool s_reinit_in_progress;
extern int s_total_recovery_failures;
extern int s_no_ip_health_cycles;
extern httpd_handle_t s_captive_portal_server;
extern esp_netif_t *ap_netif;
extern TaskHandle_t s_wifi_recovery_task;
extern bool s_mdns_service_added;

// Internal functions (app_wifi.c)
esp_err_t wifi_load_credentials(char *ssid, char *password);
esp_err_t wifi_save_credentials(const char *ssid, const char *password);
void wifi_register_event_handlers_once(void);
bool wifi_sta_has_ip(void);
void wifi_set_protocol_11ax(wifi_interface_t interface);
void wifi_disable_power_save_best_effort(void);

// Internal functions (wifi_recovery.c)
void wifi_recovery_task(void *arg);
void wifi_health_monitor_task(void *arg);
void wifi_schedule_full_reinit(void);

// Internal functions (wifi_captive_portal.c)
void wifi_init_softap(void);
