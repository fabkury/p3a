# Project Notes

## 2025-11-04 — Wi-Fi bring-up (Section 5.1)

- Hosted transport initialization: `net_init()` initializes ESP-Hosted SDIO transport (40 MHz, 4-bit bus) to communicate with ESP32-C6 coprocessor, then sets up ESP-WiFi-Remote API. Default STA and AP netifs created before Wi-Fi init. Event loop created if not already present.
- SoftAP provisioning: HTTP server listens on port 80 (default config) when provisioning starts. Simple HTML form captures SSID and password (POST or GET). Credentials saved to NVS "wifi" namespace using `storage_kv_set_str()`. Provisioning task monitors completion event and auto-tears down SoftAP after 5 minutes or on success. SoftAP SSID: "P3A-Prov", password: "p3a-setup-2024".
- STA connection flow: `net_wifi_connect()` loads credentials from NVS "wifi" namespace. If no credentials found, automatically starts provisioning. On connection, configures STA mode, sets credentials, starts Wi-Fi, and waits for IP_EVENT_STA_GOT_IP. Retry logic: max 5 retries on disconnect, exponential backoff handled by ESP-IDF.
- State management: `s_wifi_state` exported from `wifi_sta.c` to track DISCONNECTED/CONNECTING/CONNECTED/PROVISIONING states. Event handlers registered once and reused. Connection status accessible via `net_wifi_get_state()` and `net_wifi_is_connected()`.
- Integration: `net_init()` called in `app_main` after storage init. `net_wifi_connect()` called after UI initialization. Connection status logged to UART. Provisioning can be triggered manually via `net_wifi_start_provisioning()`.
- Assumptions: ESP32-C6 slave firmware pre-loaded with `esp_hosted` (per Waveshare default). SDIO pins configured via Kconfig (CONFIG_ESP_HOSTED_SDIO_*). VO4 3.3V rail enabled by `board_init()` (sufficient for SDIO). HTTP server uses default config (max 4 handlers, 4 sockets).
- Outstanding: DNS redirect for true captive portal behavior (currently HTTP-only). WPA3 SAE mode configuration per network. Network scan/selection UI (future enhancement). Certificate pinning for MQTT (tracked in §5.2).

## 2025-11-04 — Wi-Fi subsystem research

- The Waveshare ESP32-P4 Wi-Fi6 Touch LCD-4B integrates an ESP32-C6-MINI-1U module that exposes Wi-Fi 6 / BLE 5 radio capabilities to the ESP32-P4 host via a 4-line SDIO 3.0 link.[^waveshare-wiki]
- Espressif’s recommended software stack for SDIO-hosted Wi-Fi on Wi-Fi-less MCUs uses `esp_wifi_remote` (transparent `esp_wifi` proxy API) together with the `esp_hosted` transport backend. This combination keeps the familiar `esp_wifi_*` programming model while delegating all RF operations to the ESP32-C6 coprocessor.
- Target configuration: ESP32-P4 operates as the **host**; ESP32-C6 slave firmware runs the `esp_hosted` Wi-Fi service; transport = SDIO 4-bit @ 40 MHz. The host stack must therefore enable the `esp_hosted` SDIO transport and ensure VO4 3.3 V rail stays enabled for GPIO47/48 as documented in the Waveshare schematic notes.
- Outstanding confirmations: verify exact ESP32-C6 firmware revision bundled by Waveshare, and whether vendor supplies pre-flashed `esp_hosted` image or if we must provision updates during manufacturing (track in §13).

[^waveshare-wiki]: Waveshare Wiki — “ESP32-P4-WIFI6-Touch-LCD-4B”, Wi-Fi networking demo section, retrieved 2025-11-04.


