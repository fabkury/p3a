// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
// Setup: when the device has no network it can join, it becomes one.
//
// It opens an access point named "p3a-setup" and answers every DNS question
// with its own address. A phone that joins asks its operating system's
// "captive portal" probe URL, the HTTP server answers every unknown page with
// a redirect to the setup page (http_server.c asks net_setup_active()), and
// the phone opens that page by itself. The page stores the network through
// PUT /api/v2/wifi, and the device restarts and joins it.
//
// The redirect target is the address, not a name: mDNS often does not
// resolve inside a phone's sign-in sheet.

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "net_dns.h"
#include "net_internal.h"
#include "p3a_event.h"
#include "p3a_ui_hooks.h"
#include "p3a_job.h"
#include "p3a_task.h"

static const char *TAG = "net_setup";

#define SETUP_AP_SSID    "p3a-setup"
#define SETUP_AP_CHANNEL 1
#define SETUP_AP_CLIENTS 4

static const uint8_t s_ap_ip[4] = {192, 168, 4, 1};

static atomic_bool s_active;
static atomic_bool s_requested;
static TaskHandle_t s_dns_task;

bool net_setup_active(void)
{
    return atomic_load(&s_active);
}

// Answers every DNS question with this device's address, for as long as the
// device runs. Packets come from anyone who joined the open access point, so
// the parsing is in net_dns.c, where it is tested.
static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t query[512];
    static uint8_t reply[512 + NET_DNS_ANSWER_BYTES];
    const struct sockaddr_in any = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int sock = -1;
    while (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock >= 0 && bind(sock, (const struct sockaddr *)&any, sizeof(any)) < 0) {
            close(sock);
            sock = -1;
        }
        if (sock < 0) {
            ESP_LOGE(TAG, "no DNS socket; trying again in 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));  // never spin on a socket error
            continue;
        }
        size_t n = net_dns_answer(query, (size_t)len, reply, sizeof(reply), s_ap_ip);
        if (n > 0) {
            sendto(sock, reply, n, 0, (const struct sockaddr *)&from, from_len);
        }
    }
}

// What the screen says in setup: the network to join, the two addresses the
// page answers at, and whether anything has joined yet (so that a person
// whose phone shows no page knows the join itself worked).
static void show_setup_screen(int clients)
{
    p3a_ui_content_t words;
    p3a_ui_content(&words, "Wi-Fi setup", -1,
                   "Join this Wi-Fi network with a phone or a computer. The setup page opens by itself.\n\n"
                   "Network\t%s\nOr open\thttp://192.168.4.1\nor\thttp://p3a.local\nJoined\t%s",
                   SETUP_AP_SSID, clients <= 0 ? "Nobody yet" : clients == 1 ? "1 device" : "Several devices");
    snprintf(words.hint, sizeof(words.hint), "Long press to hide");
    p3a_ui_screen_show(P3A_SCREEN_SETUP, &words);
}

// On its own little network the device is simply "p3a": there is nothing
// else there to clash with, and it is the name the instructions give.
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS did not start (setup still works by address): %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("p3a");
    mdns_instance_name_set("p3a Wi-Fi setup");
    // A named device also answers to its own name here, so that the address
    // its owner knows keeps working during setup.
    if (strcmp(net_hostname(), "p3a") != 0) {
        mdns_ip_addr_t address = {.addr = {.type = ESP_IPADDR_TYPE_V4}};
        IP4_ADDR(&address.addr.u_addr.ip4, s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);
        err = mdns_delegate_hostname_add(net_hostname(), &address);
        if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "the alias %s.local was not added: %s", net_hostname(), esp_err_to_name(err));
        }
    }
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {  // INVALID_ARG: already there
        ESP_LOGW(TAG, "mDNS service not added: %s", esp_err_to_name(err));
    }
}

static esp_err_t start_access_point(void)
{
    esp_err_t err;
    if (g_net.driver_started) {
        // The station tried and failed. Stop it; the same driver becomes the
        // access point.
        err = esp_wifi_remote_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "stopping the station failed: %s", esp_err_to_name(err));
        }
    } else {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        if ((err = esp_wifi_remote_init(&init)) != ESP_OK) {
            return err;
        }
        g_net.driver_started = true;
    }
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (ap == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Open, as in 1.x: the page it serves asks for the home network's
    // password, so a password here would have to be printed on the screen
    // for anyone in the room anyway.
    wifi_config_t config = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = sizeof(SETUP_AP_SSID) - 1,
            .channel = SETUP_AP_CHANNEL,
            .max_connection = SETUP_AP_CLIENTS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    if ((err = esp_wifi_remote_set_mode(WIFI_MODE_AP)) != ESP_OK ||
        (err = esp_wifi_remote_set_config(WIFI_IF_AP, &config)) != ESP_OK ||
        (err = esp_wifi_remote_start()) != ESP_OK) {
        return err;
    }
    esp_wifi_remote_set_protocol(WIFI_IF_AP,
                                 WIFI_PROTOCOL_11AX | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11B);

    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);
    IP4_ADDR(&ip.gw, s_ap_ip[0], s_ap_ip[1], s_ap_ip[2], s_ap_ip[3]);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip);
    esp_netif_dhcps_start(ap);
    return ESP_OK;
}

static esp_err_t setup_job(void *arg)
{
    (void)arg;
    esp_err_t err = start_access_point();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the setup access point did not start: %s", esp_err_to_name(err));
        atomic_store(&s_requested, false);
        return err;
    }
    if (s_dns_task == NULL) {
        p3a_task_config_t task = P3A_TASK_CONFIG("net_dns", dns_task, NULL, 4096, P3A_TASK_NETWORK);
        if (p3a_task_create(&task, &s_dns_task) != ESP_OK) {
            ESP_LOGW(TAG, "no DNS task: the setup page will not open by itself, but http://192.168.4.1/ works");
        }
    }
    start_mdns();
    atomic_store(&s_active, true);
    ESP_LOGW(TAG, "setup: join the Wi-Fi network \"%s\" and open http://192.168.4.1/", SETUP_AP_SSID);
    show_setup_screen(0);
    p3a_event_emit(P3A_EVENT_NET_SETUP_STARTED);
    return ESP_OK;
}

void net_setup_clients_changed(int clients)
{
    if (atomic_load(&s_active)) {
        show_setup_screen(clients);
    }
}

void net_enter_setup(void)
{
    net_set_state(P3A_NET_SETUP);
    if (atomic_exchange(&s_requested, true)) {
        return;
    }
    if (g_net.reconnect_timer != NULL) {
        esp_timer_stop(g_net.reconnect_timer);
    }
    // Switching the radio over takes a second or so of exchanges with the
    // co-processor: not for the event task or for boot.
    if (p3a_job_submit(P3A_JOBS_IO, "net_setup", setup_job, NULL, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "could not queue the setup access point");
        atomic_store(&s_requested, false);
    }
}
