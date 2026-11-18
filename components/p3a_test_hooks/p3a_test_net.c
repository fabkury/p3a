// SPDX-License-Identifier: Apache-2.0
// Test console commands for the network layer and the clock.

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_wifi_remote.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
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

static void cmd_net(const char *args)
{
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
        p3a_test_reply_err("usage: net status|join <json>|forget|drop|resolve <host>");
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
