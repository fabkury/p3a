// SPDX-License-Identifier: Apache-2.0
// The network: Wi-Fi through the ESP32-C6 co-processor, the device's name on
// the LAN, and the clock.
//
// p3a_net_start() never waits for a connection. Everything after it is
// reported by events (P3A_EVENT_NET_UP, P3A_EVENT_NET_DOWN) and can be read
// with p3a_net_status(). Code that needs the network checks p3a_net_is_up()
// and copes with "no".
//
// Staying connected is this component's job, with three layers of remedy:
//   1. a dropped connection is retried, immediately at first and then with a
//      growing delay (net_backoff.h);
//   2. ten drops in a row, a long stretch without an address, or a failed
//      reachability check tear the Wi-Fi driver down and bring it up again;
//   3. a co-processor that stops answering restarts the device, a bounded
//      number of times (p3a_recovery.h), after which the device plays on
//      without a network until the next restart.
//
// Wi-Fi credentials live in their own NVS namespace, not in the settings
// store, and are never logged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P3A_NET_OFF,         // p3a_net_start() has not run, or failed
    P3A_NET_SETUP,       // no usable credentials: the device needs setting up
    P3A_NET_CONNECTING,  // trying to join, or to rejoin after a drop
    P3A_NET_UP,          // joined, with an address
    P3A_NET_DEGRADED,    // the co-processor link is dead; no network until a restart
} p3a_net_state_t;

typedef struct {
    p3a_net_state_t state;
    bool has_credentials;
    bool time_set;
    char ssid[33];       // the stored network name, "" when none
    char ip[16];         // "" unless up
    char hostname[24];   // "p3a" or "p3a-<device_name>"; reachable as <hostname>.local
    int rssi;            // dBm; 0 unless up
    uint32_t drops;      // disconnections since boot
    uint32_t reinits;    // times the Wi-Fi driver was torn down and brought back
} p3a_net_status_t;

// Brings the network stack up and, if credentials are stored, starts joining.
// Returns as soon as that is under way. Call once, after p3a_settings_start()
// and after the co-processor link is up (slave_ota).
esp_err_t p3a_net_start(void);

void p3a_net_status(p3a_net_status_t *out);
bool p3a_net_is_up(void);
// "off", "setup", "connecting", "up", "degraded".
const char *p3a_net_state_name(p3a_net_state_t state);

// Stores the network to join. Takes effect at the next restart, like every
// other way of changing it. ESP_ERR_INVALID_ARG for an SSID that is empty or
// longer than 32 bytes, or a password longer than 64.
esp_err_t p3a_net_set_credentials(const char *ssid, const char *password);

// Forgets the stored network. The next restart enters setup.
esp_err_t p3a_net_erase_credentials(void);

#ifdef __cplusplus
}
#endif
