// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "usb_descriptors.h"

#include <string.h>
#include <stdio.h>

#include "tusb.h"
#include "esp_mac.h"

#define P3A_USB_VID 0x303A
#define P3A_USB_PID 0x80A8
#define P3A_USB_BCD 0x0200

#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
#define P3A_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_VENDOR_DESC_LEN)
#else
#define P3A_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
#endif

static tusb_desc_device_t const s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = P3A_USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = P3A_USB_VID,
    .idProduct = P3A_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = P3A_STRID_MANUFACTURER,
    .iProduct = P3A_STRID_PRODUCT,
    .iSerialNumber = P3A_STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

static uint8_t const s_full_speed_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, P3A_ITF_NUM_TOTAL, 0, P3A_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_CDC_DESCRIPTOR(P3A_ITF_NUM_CDC_COMM, P3A_STRID_CDC_INTERFACE, P3A_USB_EP_CDC_NOTIF, 8,
                       P3A_USB_EP_CDC_OUT, P3A_USB_EP_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(P3A_ITF_NUM_MSC, P3A_STRID_MSC_INTERFACE, P3A_USB_EP_MSC_OUT, P3A_USB_EP_MSC_IN, 64),
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    TUD_VENDOR_DESCRIPTOR(P3A_ITF_NUM_VENDOR, P3A_STRID_VENDOR_INTERFACE,
                          P3A_USB_EP_VENDOR_OUT, P3A_USB_EP_VENDOR_IN, 64),
#endif
};

#if TUD_OPT_HIGH_SPEED
static uint8_t const s_high_speed_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, P3A_ITF_NUM_TOTAL, 0, P3A_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_CDC_DESCRIPTOR(P3A_ITF_NUM_CDC_COMM, P3A_STRID_CDC_INTERFACE, P3A_USB_EP_CDC_NOTIF, 8,
                       P3A_USB_EP_CDC_OUT, P3A_USB_EP_CDC_IN, 512),
    TUD_MSC_DESCRIPTOR(P3A_ITF_NUM_MSC, P3A_STRID_MSC_INTERFACE, P3A_USB_EP_MSC_OUT, P3A_USB_EP_MSC_IN, 512),
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    TUD_VENDOR_DESCRIPTOR(P3A_ITF_NUM_VENDOR, P3A_STRID_VENDOR_INTERFACE,
                          P3A_USB_EP_VENDOR_OUT, P3A_USB_EP_VENDOR_IN, 512),
#endif
};

static tusb_desc_device_qualifier_t const s_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = P3A_USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0x00,
};
#endif

// MAC-derived serial number (populated by usb_desc_init)
static char s_usb_serial[13] = "000000000000";

static const char *const s_string_desc_base[] = {
    (const char[]){0x09, 0x04},  // English (United States)
    "FabKury",
    "P3A Composite Bridge",
    NULL,  // Serial: filled dynamically from s_usb_serial
    "P3A CDC Console",
    "P3A SD Drive",
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    "P3A PICO-8 Stream",
#endif
};

// Writable copy that points to s_usb_serial for the serial string
static const char *s_string_desc[sizeof(s_string_desc_base) / sizeof(s_string_desc_base[0])];
static bool s_desc_initialized = false;

void usb_desc_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_usb_serial, sizeof(s_usb_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Build string descriptor table with dynamic serial
    size_t count = sizeof(s_string_desc_base) / sizeof(s_string_desc_base[0]);
    for (size_t i = 0; i < count; i++) {
        if (i == P3A_STRID_SERIAL) {
            s_string_desc[i] = s_usb_serial;
        } else {
            s_string_desc[i] = s_string_desc_base[i];
        }
    }
    s_desc_initialized = true;
}

// Descriptor accessors for tinyusb_driver_install()
const tusb_desc_device_t *usb_desc_get_device(void)
{
    return &s_device_descriptor;
}

const uint8_t *usb_desc_get_fs_configuration(void)
{
    return s_full_speed_configuration;
}

const char **usb_desc_get_string_table(size_t *count)
{
    if (!s_desc_initialized) {
        usb_desc_init();
    }
    if (count) {
        *count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
    }
    return (const char **)s_string_desc;
}

#if TUD_OPT_HIGH_SPEED
const uint8_t *usb_desc_get_hs_configuration(void)
{
    return s_high_speed_configuration;
}

const tusb_desc_device_qualifier_t *usb_desc_get_qualifier(void)
{
    return &s_device_qualifier;
}
#endif


