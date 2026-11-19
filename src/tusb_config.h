// fm1-stm32-unbrick - TinyUSB: host only, OTG FS (rhport 0), one device, no class drivers.
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#define CFG_TUSB_MCU OPT_MCU_STM32F4
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 0
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

#define CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 0
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_ENDPOINT_MAX 4
#define CFG_TUH_API_EDPT_XFER 1

// no class drivers: the MSC endpoints are driven by host.c
#define CFG_TUH_MSC 0
#define CFG_TUH_CDC 0
#define CFG_TUH_HID 0
#define CFG_TUH_VENDOR 0

#endif
