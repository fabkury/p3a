// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "usb_descriptors.h"

#include <string.h>

#include "tusb.h"

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

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_device_descriptor;
}

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

static uint8_t s_other_speed_configuration[P3A_CONFIG_TOTAL_LEN];

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

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return (uint8_t const *)&s_device_qualifier;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;

    uint8_t const *src;
    if (tud_speed_get() == TUSB_SPEED_HIGH) {
        src = s_full_speed_configuration;
    } else {
        src = s_high_speed_configuration;
    }
    
    size_t src_size = (src == s_full_speed_configuration) ? sizeof(s_full_speed_configuration) : sizeof(s_high_speed_configuration);
    size_t copy_size = (src_size < sizeof(s_other_speed_configuration)) ? src_size : sizeof(s_other_speed_configuration);
    memcpy(s_other_speed_configuration, src, copy_size);
    s_other_speed_configuration[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return s_other_speed_configuration;
}
#endif

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
#if TUD_OPT_HIGH_SPEED
    return (tud_speed_get() == TUSB_SPEED_HIGH) ? s_high_speed_configuration : s_full_speed_configuration;
#else
    return s_full_speed_configuration;
#endif
}

static const char *const s_string_desc[] = {
    (const char[]){0x09, 0x04},  // English (United States)
    "FabKury",
    "P3A Composite Bridge",
    "0001",
    "P3A CDC Console",
    "P3A SD Drive",
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    "P3A PICO-8 Stream",
#endif
};

static uint16_t s_string_buf[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count = 0;

    if (index == P3A_STRID_LANGID) {
        memcpy(&s_string_buf[1], s_string_desc[0], 2);
        chr_count = 1;
    } else {
        if (index >= (sizeof(s_string_desc) / sizeof(s_string_desc[0]))) {
            return NULL;
        }
        const char *str = s_string_desc[index];
        if (str == NULL) {
            return NULL;
        }
        chr_count = strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (size_t i = 0; i < chr_count; ++i) {
            s_string_buf[1 + i] = (uint16_t)str[i];
        }
    }

    s_string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_string_buf;
}


