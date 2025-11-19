#pragma once

#include "sdkconfig.h"
#include "tusb_option.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#endif

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_OS           OPT_OS_FREERTOS

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_MAX_SPEED       OPT_MODE_HIGH_SPEED

#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             1
#define CFG_TUD_VENDOR          1

#define CFG_TUD_CDC_RX_BUFSIZE  512
#define CFG_TUD_CDC_TX_BUFSIZE  512
#define CFG_TUD_CDC_EP_BUFSIZE  512

#define CFG_TUD_MSC_EP_BUFSIZE  512
#define CFG_TUD_MSC_TX_BUFSIZE  512
#define CFG_TUD_MSC_RX_BUFSIZE  512

#define CFG_TUD_VENDOR_TX_BUFSIZE 8192
#define CFG_TUD_VENDOR_RX_BUFSIZE 8192
#define CFG_TUD_VENDOR_EP_BUFSIZE 512

#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(16)))
#define CFG_TUSB_MEM_SECTION

#define CFG_TUSB_DEBUG              CONFIG_TINYUSB_DEBUG_LEVEL

#ifdef __cplusplus
}
#endif


