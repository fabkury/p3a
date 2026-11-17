// SPDX-License-Identifier: Apache-2.0
// UART test console. Protocol in p3a_test_hooks.h.

#include "p3a_test_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p3a_err.h"
#include "p3a_task.h"

#define CONSOLE_UART   ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define LINE_MAX_LEN   1024
#define RX_BUFFER_SIZE 2048

static const char *TAG = "p3a_test";

typedef void (*p3a_test_command_fn)(const char *args);

static void reply_ok(const char *json)
{
    printf("TEST|ok|%s\n", json);
}

static void reply_err(const char *message)
{
    printf("TEST|err|%s\n", message);
}

// {"version":...,"idf":...,"chip_rev":100,"cores":2,"mac":"AA:BB:..."}
static void format_identity(char *out, size_t out_len)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, out_len,
             "{\"version\":\"%s\",\"idf\":\"%s\",\"chip_rev\":%u,\"cores\":%u,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
             app->version, app->idf_ver, (unsigned)chip.revision, (unsigned)chip.cores,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void cmd_version(const char *args)
{
    (void)args;
    char json[192];
    format_identity(json, sizeof(json));
    reply_ok(json);
}

static void cmd_reboot(const char *args)
{
    (void)args;
    reply_ok("{}");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// Reports how the calling task (the console itself, class BACKGROUND) was
// created, so a device test can check that the task helper applies its policy.
static void cmd_task(const char *args)
{
    (void)args;
    char json[160];
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"priority\":%u,\"core\":%d,\"psram_stack\":%s}",
             pcTaskGetName(NULL), (unsigned)uxTaskPriorityGet(NULL), (int)xPortGetCoreID(),
             esp_ptr_external_ram(pxTaskGetStackStart(NULL)) ? "true" : "false");
    reply_ok(json);
}

// Maps an error code to its printable name: !errname 327682
static void cmd_errname(const char *args)
{
    char *end = NULL;
    long code = strtol(args, &end, 0);
    if (end == args) {
        reply_err("usage: !errname <code>");
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"p3a\":%s}", p3a_err_to_name((esp_err_t)code),
             p3a_err_is_p3a((esp_err_t)code) ? "true" : "false");
    reply_ok(json);
}

static const struct {
    const char *name;
    p3a_test_command_fn fn;
} s_commands[] = {
    {"version", cmd_version},
    {"reboot", cmd_reboot},
    {"task", cmd_task},
    {"errname", cmd_errname},
};

static void dispatch(char *line)
{
    if (line[0] != '!') {
        return;
    }
    char *name = line + 1;
    char *args = strchr(name, ' ');
    if (args) {
        *args++ = '\0';
    } else {
        args = name + strlen(name);
    }
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(name, s_commands[i].name) == 0) {
            s_commands[i].fn(args);
            return;
        }
    }
    reply_err("unknown command");
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[LINE_MAX_LEN];
    size_t len = 0;
    bool overflow = false;

    char ready[192];
    format_identity(ready, sizeof(ready));
    printf("TEST|ready|%s\n", ready);

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (len < sizeof(line) - 1) {
                line[len++] = (char)c;
            } else {
                overflow = true;
            }
            continue;
        }
        line[len] = '\0';
        if (overflow) {
            if (line[0] == '!') {
                reply_err("line too long");
            }
        } else if (len > 0) {
            dispatch(line);
        }
        len = 0;
        overflow = false;
    }
}

void p3a_test_console_start(void)
{
    esp_err_t err = uart_driver_install(CONSOLE_UART, RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return;
    }
    p3a_task_config_t task = P3A_TASK_CONFIG("p3a_test_con", console_task, NULL, 4096, P3A_TASK_BACKGROUND);
    err = p3a_task_create(&task, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start the test console task: %s", esp_err_to_name(err));
    }
}
