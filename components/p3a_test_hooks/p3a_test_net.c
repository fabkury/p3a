// SPDX-License-Identifier: Apache-2.0
// Test console commands for the network layer and the clock.

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "p3a_clock.h"
#include "p3a_err.h"
#include "p3a_event.h"
#include "p3a_net.h"
#include "p3a_test_internal.h"

static atomic_int s_ups;
static atomic_int s_downs;
static bool s_subscribed;

static void on_up(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    atomic_fetch_add(&s_ups, 1);
}

static void on_down(const p3a_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    atomic_fetch_add(&s_downs, 1);
}

void p3a_test_net_subscribe(void)
{
    if (!s_subscribed) {
        s_subscribed = p3a_event_subscribe(P3A_EVENT_NET_UP, on_up, NULL) == ESP_OK &&
                       p3a_event_subscribe(P3A_EVENT_NET_DOWN, on_down, NULL) == ESP_OK;
    }
}

// net status
static void cmd_status(void)
{
    p3a_net_status_t st;
    p3a_net_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", p3a_net_state_name(st.state));
    cJSON_AddBoolToObject(o, "up", p3a_net_is_up());
    cJSON_AddBoolToObject(o, "has_credentials", st.has_credentials);
    cJSON_AddStringToObject(o, "ssid", st.ssid);
    cJSON_AddStringToObject(o, "ip", st.ip);
    cJSON_AddStringToObject(o, "hostname", st.hostname);
    cJSON_AddNumberToObject(o, "rssi", st.rssi);
    cJSON_AddNumberToObject(o, "drops", st.drops);
    cJSON_AddNumberToObject(o, "reinits", st.reinits);
    cJSON_AddBoolToObject(o, "time_set", st.time_set);
    cJSON_AddNumberToObject(o, "up_events", atomic_load(&s_ups));
    cJSON_AddNumberToObject(o, "down_events", atomic_load(&s_downs));
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text == NULL) {
        p3a_test_reply_err("no memory");
        return;
    }
    p3a_test_reply_ok(text);
    cJSON_free(text);
}

// net join {"ssid":"...","password":"..."}: stores the network. Nothing of it
// is echoed back.
static void cmd_join(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(o, "password");
    esp_err_t err = cJSON_IsString(ssid) && cJSON_IsString(password)
                        ? p3a_net_set_credentials(ssid->valuestring, password->valuestring)
                        : ESP_ERR_INVALID_ARG;
    cJSON_Delete(o);
    if (err != ESP_OK) {
        p3a_test_reply_err(p3a_err_to_name(err));
        return;
    }
    p3a_test_reply_ok("{}");
}

// net resolve <host>: one DNS lookup, as proof that the internet is there.
static void cmd_resolve(const char *host)
{
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    int err = getaddrinfo(host, "80", &hints, &result);
    char json[96];
    if (err == 0 && result != NULL) {
        char ip[16];
        inet_ntoa_r(((struct sockaddr_in *)result->ai_addr)->sin_addr, ip, sizeof(ip));
        snprintf(json, sizeof(json), "{\"resolved\":true,\"ip\":\"%s\"}", ip);
    } else {
        snprintf(json, sizeof(json), "{\"resolved\":false,\"error\":%d}", err);
    }
    if (result != NULL) {
        freeaddrinfo(result);
    }
    p3a_test_reply_ok(json);
}

// net setup: what the setup access point looks like from inside.
static void cmd_setup(void)
{
    char ip[16] = "";
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (ap != NULL && esp_netif_get_ip_info(ap, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_config_t config = {0};
    bool is_ap = esp_wifi_remote_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP &&
                 esp_wifi_remote_get_config(WIFI_IF_AP, &config) == ESP_OK;
    char json[160];
    snprintf(json, sizeof(json), "{\"ap\":%s,\"ssid\":\"%.32s\",\"open\":%s,\"channel\":%d,\"ip\":\"%s\"}",
             is_ap ? "true" : "false", is_ap ? (const char *)config.ap.ssid : "",
             is_ap && config.ap.authmode == WIFI_AUTH_OPEN ? "true" : "false", is_ap ? config.ap.channel : 0, ip);
    p3a_test_reply_ok(json);
}

// net selfdns <name>: asks the device's own DNS catch-all for a name, over
// the loopback interface, as a phone on the setup network would.
static void cmd_selfdns(const char *name)
{
    uint8_t packet[300] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01};
    size_t at = 12;
    const char *label = name;
    while (*label != '\0' && at < 200) {
        const char *dot = strchr(label, '.');
        size_t len = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (len == 0 || len > 63) {
            break;
        }
        packet[at++] = (uint8_t)len;
        memcpy(packet + at, label, len);
        at += len;
        label += len + (dot != NULL ? 1 : 0);
    }
    packet[at++] = 0;
    packet[at++] = 0;
    packet[at++] = 1;  // type A
    packet[at++] = 0;
    packet[at++] = 1;  // class IN

    char json[128] = "{\"answered\":false}";
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        const struct timeval wait = {.tv_sec = 2};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(53)};
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(sock, packet, at, 0, (const struct sockaddr *)&to, sizeof(to));
        uint8_t reply[320];
        int n = recv(sock, reply, sizeof(reply), 0);
        if (n >= (int)at + 16 && reply[0] == 0x12 && reply[1] == 0x34) {
            const uint8_t *ip = reply + n - 4;
            snprintf(json, sizeof(json), "{\"answered\":true,\"answers\":%d,\"ip\":\"%d.%d.%d.%d\",\"bytes\":%d}",
                     (reply[6] << 8) | reply[7], ip[0], ip[1], ip[2], ip[3], n);
        }
        close(sock);
    }
    p3a_test_reply_ok(json);
}

// net selfhttp <path>: a GET to the device's own HTTP server over loopback,
// reporting the status line and the headers a portal test cares about.
static void cmd_selfhttp(const char *path)
{
    static char buf[1024];
    int status = 0;
    char location[64] = "";
    char encoding[16] = "";
    char type[48] = "";
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock >= 0) {
        const struct timeval wait = {.tv_sec = 4};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(80)};
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(sock, (const struct sockaddr *)&to, sizeof(to)) == 0) {
            int len = snprintf(buf, sizeof(buf), "GET %.200s HTTP/1.1\r\nHost: probe.example\r\nConnection: close\r\n\r\n", path);
            send(sock, buf, (size_t)len, 0);
            int got = 0;
            while (got < (int)sizeof(buf) - 1) {
                int n = recv(sock, buf + got, sizeof(buf) - 1 - (size_t)got, 0);
                if (n <= 0) {
                    break;
                }
                got += n;
            }
            buf[got] = '\0';
            sscanf(buf, "HTTP/1.1 %d", &status);
            const char *h;
            if ((h = strstr(buf, "\r\nLocation: ")) != NULL) {
                sscanf(h + 12, "%63[^\r]", location);
            }
            if ((h = strstr(buf, "\r\nContent-Encoding: ")) != NULL) {
                sscanf(h + 20, "%15[^\r]", encoding);
            }
            if ((h = strstr(buf, "\r\nContent-Type: ")) != NULL) {
                sscanf(h + 16, "%47[^\r]", type);
            }
        }
        close(sock);
    }
    char json[256];
    snprintf(json, sizeof(json), "{\"status\":%d,\"location\":\"%s\",\"encoding\":\"%s\",\"type\":\"%s\"}", status, location,
             encoding, type);
    p3a_test_reply_ok(json);
}

static void cmd_net(const char *args)
{
    if (strcmp(args, "setup") == 0) {
        cmd_setup();
        return;
    }
    if (strncmp(args, "selfdns ", 8) == 0) {
        cmd_selfdns(args + 8);
        return;
    }
    if (strncmp(args, "selfhttp ", 9) == 0) {
        cmd_selfhttp(args + 9);
        return;
    }
    if (strcmp(args, "status") == 0) {
        cmd_status();
    } else if (strncmp(args, "join ", 5) == 0) {
        cmd_join(args + 5);
    } else if (strcmp(args, "forget") == 0) {
        esp_err_t err = p3a_net_erase_credentials();
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
        } else {
            p3a_test_reply_ok("{}");
        }
    } else if (strcmp(args, "drop") == 0) {
        // Disconnects from the access point; the device should rejoin by itself.
        esp_err_t err = esp_wifi_remote_disconnect();
        if (err != ESP_OK) {
            p3a_test_reply_err(p3a_err_to_name(err));
        } else {
            p3a_test_reply_ok("{}");
        }
    } else if (strncmp(args, "resolve ", 8) == 0) {
        cmd_resolve(args + 8);
    } else {
        p3a_test_reply_err("usage: net status|join <json>|forget|drop|resolve <host>|setup|selfdns <name>|selfhttp <path>");
    }
}

// clock: whether the clock is set, and what it says.
static void cmd_clock(const char *args)
{
    (void)args;
    char now[P3A_CLOCK_ISO8601_LEN] = "";
    esp_err_t err = p3a_clock_now_iso8601(now, sizeof(now));
    char json[128];
    snprintf(json, sizeof(json), "{\"set\":%s,\"now\":\"%s\",\"unix\":%lld,\"error\":\"%s\"}",
             p3a_clock_is_set() ? "true" : "false", now, (long long)time(NULL), err == ESP_OK ? "" : p3a_err_to_name(err));
    p3a_test_reply_ok(json);
}

const p3a_test_command_t p3a_test_net_commands[] = {
    {"net", cmd_net},
    {"clock", cmd_clock},
};
const size_t p3a_test_net_command_count = sizeof(p3a_test_net_commands) / sizeof(p3a_test_net_commands[0]);
