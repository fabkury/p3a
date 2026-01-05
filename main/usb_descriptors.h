// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

/*
 * USB descriptor definitions for the P3A composite device.
 */

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    P3A_ITF_NUM_CDC_COMM = 0,
    P3A_ITF_NUM_CDC_DATA,
    P3A_ITF_NUM_MSC,
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    P3A_ITF_NUM_VENDOR,
#endif
    P3A_ITF_NUM_TOTAL,
};

#define P3A_USB_EP_CDC_NOTIF   0x81
#define P3A_USB_EP_CDC_OUT     0x02
#define P3A_USB_EP_CDC_IN      0x82
#define P3A_USB_EP_MSC_OUT     0x03
#define P3A_USB_EP_MSC_IN      0x83
#define P3A_USB_EP_VENDOR_OUT  0x04
#define P3A_USB_EP_VENDOR_IN   0x84

enum {
    P3A_STRID_LANGID = 0,
    P3A_STRID_MANUFACTURER,
    P3A_STRID_PRODUCT,
    P3A_STRID_SERIAL,
    P3A_STRID_CDC_INTERFACE,
    P3A_STRID_MSC_INTERFACE,
#if CONFIG_P3A_PICO8_USB_STREAM_ENABLE
    P3A_STRID_VENDOR_INTERFACE,
#endif
};

// Descriptor accessors for tinyusb_driver_install()
#include "tusb.h"

const tusb_desc_device_t *usb_desc_get_device(void);
const uint8_t *usb_desc_get_fs_configuration(void);
const char **usb_desc_get_string_table(size_t *count);
#if TUD_OPT_HIGH_SPEED
const uint8_t *usb_desc_get_hs_configuration(void);
const tusb_desc_device_qualifier_t *usb_desc_get_qualifier(void);
#endif

#ifdef __cplusplus
}
#endif

