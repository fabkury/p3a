/*
 * USB descriptor definitions for the P3A composite device.
 */

#pragma once

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

#ifdef __cplusplus
}
#endif

